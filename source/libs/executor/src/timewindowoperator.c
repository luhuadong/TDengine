/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include "executorimpl.h"
#include "function.h"
#include "functionMgt.h"
#include "tdatablock.h"
#include "ttime.h"
#include "tfill.h"

typedef enum SResultTsInterpType {
  RESULT_ROW_START_INTERP = 1,
  RESULT_ROW_END_INTERP = 2,
} SResultTsInterpType;

static SSDataBlock* doStreamSessionAgg(SOperatorInfo* pOperator);

static int64_t* extractTsCol(SSDataBlock* pBlock, const SIntervalAggOperatorInfo* pInfo);

static SResultRowPosition addToOpenWindowList(SResultRowInfo* pResultRowInfo, const SResultRow* pResult);
static void doCloseWindow(SResultRowInfo* pResultRowInfo, const SIntervalAggOperatorInfo* pInfo, SResultRow* pResult);

///*
// * There are two cases to handle:
// *
// * 1. Query range is not set yet (queryRangeSet = 0). we need to set the query range info, including
// * pQueryAttr->lastKey, pQueryAttr->window.skey, and pQueryAttr->eKey.
// * 2. Query range is set and query is in progress. There may be another result with the same query ranges to be
// *    merged during merge stage. In this case, we need the pTableQueryInfo->lastResRows to decide if there
// *    is a previous result generated or not.
// */
//static void setIntervalQueryRange(STableQueryInfo* pTableQueryInfo, TSKEY key, STimeWindow* pQRange) {
//  // do nothing
//}

static TSKEY getStartTsKey(STimeWindow* win, const TSKEY* tsCols) { return tsCols == NULL ? win->skey : tsCols[0]; }

static void getInitialStartTimeWindow(SInterval* pInterval, int32_t precision, TSKEY ts, STimeWindow* w,
                                      bool ascQuery) {
  if (ascQuery) {
    getAlignQueryTimeWindow(pInterval, precision, ts, w);
  } else {
    // the start position of the first time window in the endpoint that spreads beyond the queried last timestamp
    getAlignQueryTimeWindow(pInterval, precision, ts, w);

    int64_t key = w->skey;
    while (key < ts) {  // moving towards end
      key = taosTimeAdd(key, pInterval->sliding, pInterval->slidingUnit, precision);
      if (key >= ts) {
        break;
      }

      w->skey = key;
    }
  }
}

// get the correct time window according to the handled timestamp
STimeWindow getActiveTimeWindow(SDiskbasedBuf* pBuf, SResultRowInfo* pResultRowInfo, int64_t ts, SInterval* pInterval,
                                int32_t precision, STimeWindow* win) {
  STimeWindow w = {0};

  if (pResultRowInfo->cur.pageId == -1) {  // the first window, from the previous stored value
    getInitialStartTimeWindow(pInterval, precision, ts, &w, true);
    w.ekey = taosTimeAdd(w.skey, pInterval->interval, pInterval->intervalUnit, precision) - 1;
  } else {
    w = getResultRowByPos(pBuf, &pResultRowInfo->cur)->win;
  }

  if (w.skey > ts || w.ekey < ts) {
    if (pInterval->intervalUnit == 'n' || pInterval->intervalUnit == 'y') {
      w.skey = taosTimeTruncate(ts, pInterval, precision);
      w.ekey = taosTimeAdd(w.skey, pInterval->interval, pInterval->intervalUnit, precision) - 1;
    } else {
      int64_t st = w.skey;

      if (st > ts) {
        st -= ((st - ts + pInterval->sliding - 1) / pInterval->sliding) * pInterval->sliding;
      }

      int64_t et = st + pInterval->interval - 1;
      if (et < ts) {
        st += ((ts - et + pInterval->sliding - 1) / pInterval->sliding) * pInterval->sliding;
      }

      w.skey = st;
      w.ekey = taosTimeAdd(w.skey, pInterval->interval, pInterval->intervalUnit, precision) - 1;
    }
  }
  return w;
}

static int32_t setTimeWindowOutputBuf(SResultRowInfo* pResultRowInfo, STimeWindow* win, bool masterscan,
                                      SResultRow** pResult, int64_t tableGroupId, SqlFunctionCtx* pCtx,
                                      int32_t numOfOutput, int32_t* rowCellInfoOffset, SAggSupporter* pAggSup,
                                      SExecTaskInfo* pTaskInfo) {
  assert(win->skey <= win->ekey);
  SResultRow* pResultRow = doSetResultOutBufByKey(pAggSup->pResultBuf, pResultRowInfo, (char*)&win->skey, TSDB_KEYSIZE,
                                                  masterscan, tableGroupId, pTaskInfo, true, pAggSup);

  if (pResultRow == NULL) {
    *pResult = NULL;
    return TSDB_CODE_SUCCESS;
  }

  // set time window for current result
  pResultRow->win = (*win);

  *pResult = pResultRow;
  setResultRowInitCtx(pResultRow, pCtx, numOfOutput, rowCellInfoOffset);

  return TSDB_CODE_SUCCESS;
}

static void updateTimeWindowInfo(SColumnInfoData* pColData, STimeWindow* pWin, bool includeEndpoint) {
  int64_t* ts = (int64_t*)pColData->pData;
  int32_t  delta = includeEndpoint ? 1 : 0;

  int64_t duration = pWin->ekey - pWin->skey + delta;
  ts[2] = duration;            // set the duration
  ts[3] = pWin->skey;          // window start key
  ts[4] = pWin->ekey + delta;  // window end key
}

static void doKeepTuple(SWindowRowsSup* pRowSup, int64_t ts) {
  pRowSup->win.ekey = ts;
  pRowSup->prevTs = ts;
  pRowSup->numOfRows += 1;
}

static void doKeepNewWindowStartInfo(SWindowRowsSup* pRowSup, const int64_t* tsList, int32_t rowIndex) {
  pRowSup->startRowIndex = rowIndex;
  pRowSup->numOfRows = 0;
  pRowSup->win.skey = tsList[rowIndex];
}

static FORCE_INLINE int32_t getForwardStepsInBlock(int32_t numOfRows, __block_search_fn_t searchFn, TSKEY ekey,
                                                   int16_t pos, int16_t order, int64_t* pData) {
  int32_t forwardRows = 0;

  if (order == TSDB_ORDER_ASC) {
    int32_t end = searchFn((char*)&pData[pos], numOfRows - pos, ekey, order);
    if (end >= 0) {
      forwardRows = end;

      if (pData[end + pos] == ekey) {
        forwardRows += 1;
      }
    }
  } else {
    int32_t end = searchFn((char*)&pData[pos], numOfRows - pos, ekey, order);
    if (end >= 0) {
      forwardRows = end;

      if (pData[end + pos] == ekey) {
        forwardRows += 1;
      }
    }
    //    int32_t end = searchFn((char*)pData, pos + 1, ekey, order);
    //    if (end >= 0) {
    //      forwardRows = pos - end;
    //
    //      if (pData[end] == ekey) {
    //        forwardRows += 1;
    //      }
    //    }
  }

  assert(forwardRows >= 0);
  return forwardRows;
}

int32_t binarySearchForKey(char* pValue, int num, TSKEY key, int order) {
  int32_t midPos = -1;
  int32_t numOfRows;

  if (num <= 0) {
    return -1;
  }

  assert(order == TSDB_ORDER_ASC || order == TSDB_ORDER_DESC);

  TSKEY*  keyList = (TSKEY*)pValue;
  int32_t firstPos = 0;
  int32_t lastPos = num - 1;

  if (order == TSDB_ORDER_DESC) {
    // find the first position which is smaller than the key
    while (1) {
      if (key >= keyList[firstPos]) return firstPos;
      if (key == keyList[lastPos]) return lastPos;

      if (key < keyList[lastPos]) {
        lastPos += 1;
        if (lastPos >= num) {
          return -1;
        } else {
          return lastPos;
        }
      }

      numOfRows = lastPos - firstPos + 1;
      midPos = (numOfRows >> 1) + firstPos;

      if (key < keyList[midPos]) {
        firstPos = midPos + 1;
      } else if (key > keyList[midPos]) {
        lastPos = midPos - 1;
      } else {
        break;
      }
    }

  } else {
    // find the first position which is bigger than the key
    while (1) {
      if (key <= keyList[firstPos]) return firstPos;
      if (key == keyList[lastPos]) return lastPos;

      if (key > keyList[lastPos]) {
        lastPos = lastPos + 1;
        if (lastPos >= num)
          return -1;
        else
          return lastPos;
      }

      numOfRows = lastPos - firstPos + 1;
      midPos = (numOfRows >> 1u) + firstPos;

      if (key < keyList[midPos]) {
        lastPos = midPos - 1;
      } else if (key > keyList[midPos]) {
        firstPos = midPos + 1;
      } else {
        break;
      }
    }
  }

  return midPos;
}

int32_t getNumOfRowsInTimeWindow(SDataBlockInfo* pDataBlockInfo, TSKEY* pPrimaryColumn, int32_t startPos, TSKEY ekey,
                                 __block_search_fn_t searchFn, STableQueryInfo* item, int32_t order) {
  assert(startPos >= 0 && startPos < pDataBlockInfo->rows);

  int32_t num = -1;
  int32_t step = GET_FORWARD_DIRECTION_FACTOR(order);

  if (order == TSDB_ORDER_ASC) {
    if (ekey < pDataBlockInfo->window.ekey && pPrimaryColumn) {
      num = getForwardStepsInBlock(pDataBlockInfo->rows, searchFn, ekey, startPos, order, pPrimaryColumn);
      if (item != NULL) {
        item->lastKey = pPrimaryColumn[startPos + (num - 1)] + step;
      }
    } else {
      num = pDataBlockInfo->rows - startPos;
      if (item != NULL) {
        item->lastKey = pDataBlockInfo->window.ekey + step;
      }
    }
  } else {  // desc
    if (ekey > pDataBlockInfo->window.skey && pPrimaryColumn) {
      num = getForwardStepsInBlock(pDataBlockInfo->rows, searchFn, ekey, startPos, order, pPrimaryColumn);
      if (item != NULL) {
        item->lastKey = pPrimaryColumn[startPos + (num - 1)] + step;
      }
    } else {
      num = pDataBlockInfo->rows - startPos;
      if (item != NULL) {
        item->lastKey = pDataBlockInfo->window.ekey + step;
      }
    }
  }

  assert(num >= 0);
  return num;
}

static void getNextTimeWindow(SInterval* pInterval, int32_t precision, int32_t order, STimeWindow* tw) {
  int32_t factor = GET_FORWARD_DIRECTION_FACTOR(order);
  if (pInterval->intervalUnit != 'n' && pInterval->intervalUnit != 'y') {
    tw->skey += pInterval->sliding * factor;
    tw->ekey = tw->skey + pInterval->interval - 1;
    return;
  }

  int64_t key = tw->skey, interval = pInterval->interval;
  // convert key to second
  key = convertTimePrecision(key, precision, TSDB_TIME_PRECISION_MILLI) / 1000;

  if (pInterval->intervalUnit == 'y') {
    interval *= 12;
  }

  struct tm tm;
  time_t    t = (time_t)key;
  taosLocalTime(&t, &tm);

  int mon = (int)(tm.tm_year * 12 + tm.tm_mon + interval * factor);
  tm.tm_year = mon / 12;
  tm.tm_mon = mon % 12;
  tw->skey = convertTimePrecision((int64_t)taosMktime(&tm) * 1000L, TSDB_TIME_PRECISION_MILLI, precision);

  mon = (int)(mon + interval);
  tm.tm_year = mon / 12;
  tm.tm_mon = mon % 12;
  tw->ekey = convertTimePrecision((int64_t)taosMktime(&tm) * 1000L, TSDB_TIME_PRECISION_MILLI, precision);

  tw->ekey -= 1;
}

void doTimeWindowInterpolation(SIntervalAggOperatorInfo* pInfo, int32_t numOfExprs, SArray* pDataBlock, TSKEY prevTs,
                               int32_t prevRowIndex, TSKEY curTs, int32_t curRowIndex, TSKEY windowKey, int32_t type) {
  SqlFunctionCtx* pCtx = pInfo->binfo.pCtx;

  int32_t index = 1;
  for (int32_t k = 0; k < numOfExprs; ++k) {
    if (!fmIsIntervalInterpoFunc(pCtx[k].functionId)) {
      pCtx[k].start.key = INT64_MIN;
      continue;
    }

    SFunctParam*     pParam = &pCtx[k].param[0];
    SColumnInfoData* pColInfo = taosArrayGet(pDataBlock, pParam->pCol->slotId);

    ASSERT(pColInfo->info.type == pParam->pCol->type && curTs != windowKey);

    double v1 = 0, v2 = 0, v = 0;
    if (prevRowIndex == -1) {
      SGroupKeys* p = taosArrayGet(pInfo->pPrevValues, index);
      GET_TYPED_DATA(v1, double, pColInfo->info.type, p->pData);
    } else {
      GET_TYPED_DATA(v1, double, pColInfo->info.type, colDataGetData(pColInfo, prevRowIndex));
    }

    GET_TYPED_DATA(v2, double, pColInfo->info.type, colDataGetData(pColInfo, curRowIndex));

#if 0
    if (functionId == FUNCTION_INTERP) {
      if (type == RESULT_ROW_START_INTERP) {
        pCtx[k].start.key = prevTs;
        pCtx[k].start.val = v1;

        pCtx[k].end.key = curTs;
        pCtx[k].end.val = v2;

        if (pColInfo->info.type == TSDB_DATA_TYPE_BINARY || pColInfo->info.type == TSDB_DATA_TYPE_NCHAR) {
          if (prevRowIndex == -1) {
            //            pCtx[k].start.ptr = (char*)pRuntimeEnv->prevRow[index];
          } else {
            pCtx[k].start.ptr = (char*)pColInfo->pData + prevRowIndex * pColInfo->info.bytes;
          }

          pCtx[k].end.ptr = (char*)pColInfo->pData + curRowIndex * pColInfo->info.bytes;
        }
      }
    } else if (functionId == FUNCTION_TWA) {
#endif

    SPoint point1 = (SPoint){.key = prevTs, .val = &v1};
    SPoint point2 = (SPoint){.key = curTs, .val = &v2};
    SPoint point = (SPoint){.key = windowKey, .val = &v};

    taosGetLinearInterpolationVal(&point, TSDB_DATA_TYPE_DOUBLE, &point1, &point2, TSDB_DATA_TYPE_DOUBLE);

    if (type == RESULT_ROW_START_INTERP) {
      pCtx[k].start.key = point.key;
      pCtx[k].start.val = v;
    } else {
      pCtx[k].end.key = point.key;
      pCtx[k].end.val = v;
    }

    index += 1;
  }
#if 0
  }
#endif
}

static void setNotInterpoWindowKey(SqlFunctionCtx* pCtx, int32_t numOfOutput, int32_t type) {
  if (type == RESULT_ROW_START_INTERP) {
    for (int32_t k = 0; k < numOfOutput; ++k) {
      pCtx[k].start.key = INT64_MIN;
    }
  } else {
    for (int32_t k = 0; k < numOfOutput; ++k) {
      pCtx[k].end.key = INT64_MIN;
    }
  }
}

static bool setTimeWindowInterpolationStartTs(SIntervalAggOperatorInfo* pInfo, SqlFunctionCtx* pCtx, int32_t numOfExprs,
                                              int32_t pos, SSDataBlock* pBlock, const TSKEY* tsCols, STimeWindow* win) {
  bool ascQuery = (pInfo->order == TSDB_ORDER_ASC);

  TSKEY curTs = tsCols[pos];

  SGroupKeys* pTsKey = taosArrayGet(pInfo->pPrevValues, 0);
  TSKEY       lastTs = *(int64_t*)pTsKey->pData;

  // lastTs == INT64_MIN and pos == 0 means this is the first time window, interpolation is not needed.
  // start exactly from this point, no need to do interpolation
  TSKEY key = ascQuery ? win->skey : win->ekey;
  if (key == curTs) {
    setNotInterpoWindowKey(pCtx, numOfExprs, RESULT_ROW_START_INTERP);
    return true;
  }

  // it is the first time window, no need to do interpolation
  if (pTsKey->isNull && pos == 0) {
    setNotInterpoWindowKey(pCtx, numOfExprs, RESULT_ROW_START_INTERP);
  } else {
    TSKEY prevTs = ((pos == 0) ? lastTs : tsCols[pos - 1]);
    doTimeWindowInterpolation(pInfo, numOfExprs, pBlock->pDataBlock, prevTs, pos - 1, curTs, pos, key,
                              RESULT_ROW_START_INTERP);
  }

  return true;
}

static bool setTimeWindowInterpolationEndTs(SIntervalAggOperatorInfo* pInfo, SqlFunctionCtx* pCtx, int32_t numOfExprs,
                                            int32_t endRowIndex, SArray* pDataBlock, const TSKEY* tsCols,
                                            TSKEY blockEkey, STimeWindow* win) {
  int32_t order = pInfo->order;

  TSKEY actualEndKey = tsCols[endRowIndex];
  TSKEY key = (order == TSDB_ORDER_ASC) ? win->ekey : win->skey;

  // not ended in current data block, do not invoke interpolation
  if ((key > blockEkey && (order == TSDB_ORDER_ASC)) || (key < blockEkey && (order == TSDB_ORDER_DESC))) {
    setNotInterpoWindowKey(pCtx, numOfExprs, RESULT_ROW_END_INTERP);
    return false;
  }

  // there is actual end point of current time window, no interpolation needs
  if (key == actualEndKey) {
    setNotInterpoWindowKey(pCtx, numOfExprs, RESULT_ROW_END_INTERP);
    return true;
  }

  int32_t nextRowIndex = endRowIndex + 1;
  assert(nextRowIndex >= 0);

  TSKEY nextKey = tsCols[nextRowIndex];
  doTimeWindowInterpolation(pInfo, numOfExprs, pDataBlock, actualEndKey, endRowIndex, nextKey, nextRowIndex, key,
                            RESULT_ROW_END_INTERP);
  return true;
}

static int32_t getNextQualifiedWindow(SInterval* pInterval, STimeWindow* pNext, SDataBlockInfo* pDataBlockInfo,
                                      TSKEY* primaryKeys, int32_t prevPosition, int32_t order) {
  bool ascQuery = (order == TSDB_ORDER_ASC);

  int32_t precision = pInterval->precision;
  getNextTimeWindow(pInterval, precision, order, pNext);

  // next time window is not in current block
  if ((pNext->skey > pDataBlockInfo->window.ekey && order == TSDB_ORDER_ASC) ||
      (pNext->ekey < pDataBlockInfo->window.skey && order == TSDB_ORDER_DESC)) {
    return -1;
  }

  TSKEY   skey = ascQuery ? pNext->skey : pNext->ekey;
  int32_t startPos = 0;

  // tumbling time window query, a special case of sliding time window query
  if (pInterval->sliding == pInterval->interval && prevPosition != -1) {
    startPos = prevPosition + 1;
  } else {
    if ((skey <= pDataBlockInfo->window.skey && ascQuery) || (skey >= pDataBlockInfo->window.ekey && !ascQuery)) {
      startPos = 0;
    } else {
      startPos = binarySearchForKey((char*)primaryKeys, pDataBlockInfo->rows, skey, order);
    }
  }

  /* interp query with fill should not skip time window */
  //  if (pQueryAttr->pointInterpQuery && pQueryAttr->fillType != TSDB_FILL_NONE) {
  //    return startPos;
  //  }

  /*
   * This time window does not cover any data, try next time window,
   * this case may happen when the time window is too small
   */
  if (primaryKeys == NULL) {
    if (ascQuery) {
      assert(pDataBlockInfo->window.skey <= pNext->ekey);
    } else {
      assert(pDataBlockInfo->window.ekey >= pNext->skey);
    }
  } else {
    if (ascQuery && primaryKeys[startPos] > pNext->ekey) {
      TSKEY next = primaryKeys[startPos];
      if (pInterval->intervalUnit == 'n' || pInterval->intervalUnit == 'y') {
        pNext->skey = taosTimeTruncate(next, pInterval, precision);
        pNext->ekey = taosTimeAdd(pNext->skey, pInterval->interval, pInterval->intervalUnit, precision) - 1;
      } else {
        pNext->ekey += ((next - pNext->ekey + pInterval->sliding - 1) / pInterval->sliding) * pInterval->sliding;
        pNext->skey = pNext->ekey - pInterval->interval + 1;
      }
    } else if ((!ascQuery) && primaryKeys[startPos] < pNext->skey) {
      TSKEY next = primaryKeys[startPos];
      if (pInterval->intervalUnit == 'n' || pInterval->intervalUnit == 'y') {
        pNext->skey = taosTimeTruncate(next, pInterval, precision);
        pNext->ekey = taosTimeAdd(pNext->skey, pInterval->interval, pInterval->intervalUnit, precision) - 1;
      } else {
        pNext->skey -= ((pNext->skey - next + pInterval->sliding - 1) / pInterval->sliding) * pInterval->sliding;
        pNext->ekey = pNext->skey + pInterval->interval - 1;
      }
    }
  }

  return startPos;
}

static bool isResultRowInterpolated(SResultRow* pResult, SResultTsInterpType type) {
  ASSERT(pResult != NULL && (type == RESULT_ROW_START_INTERP || type == RESULT_ROW_END_INTERP));
  if (type == RESULT_ROW_START_INTERP) {
    return pResult->startInterp == true;
  } else {
    return pResult->endInterp == true;
  }
}

static void setResultRowInterpo(SResultRow* pResult, SResultTsInterpType type) {
  assert(pResult != NULL && (type == RESULT_ROW_START_INTERP || type == RESULT_ROW_END_INTERP));
  if (type == RESULT_ROW_START_INTERP) {
    pResult->startInterp = true;
  } else {
    pResult->endInterp = true;
  }
}

static void doWindowBorderInterpolation(SIntervalAggOperatorInfo* pInfo, SSDataBlock* pBlock, int32_t numOfExprs,
                                        SqlFunctionCtx* pCtx, SResultRow* pResult, STimeWindow* win, int32_t startPos,
                                        int32_t forwardRows) {
  if (!pInfo->timeWindowInterpo) {
    return;
  }

  ASSERT(pBlock != NULL);
  if (pBlock->pDataBlock == NULL) {
    //    tscError("pBlock->pDataBlock == NULL");
    return;
  }

  SColumnInfoData* pColInfo = taosArrayGet(pBlock->pDataBlock, pInfo->primaryTsIndex);

  TSKEY* tsCols = (TSKEY*)(pColInfo->pData);
  bool   done = isResultRowInterpolated(pResult, RESULT_ROW_START_INTERP);
  if (!done) {  // it is not interpolated, now start to generated the interpolated value
    bool interp = setTimeWindowInterpolationStartTs(pInfo, pCtx, numOfExprs, startPos, pBlock, tsCols, win);
    if (interp) {
      setResultRowInterpo(pResult, RESULT_ROW_START_INTERP);
    }
  } else {
    setNotInterpoWindowKey(pCtx, numOfExprs, RESULT_ROW_START_INTERP);
  }

  // point interpolation does not require the end key time window interpolation.
  //  if (pointInterpQuery) {
  //    return;
  //  }

  // interpolation query does not generate the time window end interpolation
  done = isResultRowInterpolated(pResult, RESULT_ROW_END_INTERP);
  if (!done) {
    int32_t endRowIndex = startPos + forwardRows - 1;

    TSKEY endKey = (pInfo->order == TSDB_ORDER_ASC) ? pBlock->info.window.ekey : pBlock->info.window.skey;
    bool  interp =
        setTimeWindowInterpolationEndTs(pInfo, pCtx, numOfExprs, endRowIndex, pBlock->pDataBlock, tsCols, endKey, win);
    if (interp) {
      setResultRowInterpo(pResult, RESULT_ROW_END_INTERP);
    }
  } else {
    setNotInterpoWindowKey(pCtx, numOfExprs, RESULT_ROW_END_INTERP);
  }
}

static void saveDataBlockLastRow(SArray* pPrevKeys, const SSDataBlock* pBlock, SArray* pCols) {
  if (pBlock->pDataBlock == NULL) {
    return;
  }

  size_t num = taosArrayGetSize(pPrevKeys);
  for (int32_t k = 0; k < num; ++k) {
    SColumn* pc = taosArrayGet(pCols, k);

    SColumnInfoData* pColInfo = taosArrayGet(pBlock->pDataBlock, pc->slotId);

    SGroupKeys* pkey = taosArrayGet(pPrevKeys, k);
    for (int32_t i = pBlock->info.rows - 1; i >= 0; --i) {
      if (colDataIsNull_s(pColInfo, i)) {
        continue;
      }

      char* val = colDataGetData(pColInfo, i);
      if (IS_VAR_DATA_TYPE(pkey->type)) {
        memcpy(pkey->pData, val, varDataTLen(val));
        ASSERT(varDataTLen(val) <= pkey->bytes);
      } else {
        memcpy(pkey->pData, val, pkey->bytes);
      }

      break;
    }
  }
}

static void doInterpUnclosedTimeWindow(SOperatorInfo* pOperatorInfo, int32_t numOfExprs, SResultRowInfo* pResultRowInfo,
                                       SSDataBlock* pBlock, int32_t scanFlag, int64_t* tsCols, SResultRowPosition* p) {
  SExecTaskInfo* pTaskInfo = pOperatorInfo->pTaskInfo;

  SIntervalAggOperatorInfo* pInfo = (SIntervalAggOperatorInfo*)pOperatorInfo->info;

  int32_t  startPos = 0;
  int32_t  numOfOutput = pOperatorInfo->numOfExprs;
  uint64_t groupId = pBlock->info.groupId;

  SResultRow* pResult = NULL;

  while (1) {
    SListNode* pn = tdListGetHead(pResultRowInfo->openWindow);

    SResultRowPosition* p1 = (SResultRowPosition*)pn->data;
    if (p->pageId == p1->pageId && p->offset == p1->offset) {
      break;
    }

    SResultRow* pr = getResultRowByPos(pInfo->aggSup.pResultBuf, p1);
    ASSERT(pr->offset == p1->offset && pr->pageId == p1->pageId);

    if (pr->closed) {
      ASSERT(isResultRowInterpolated(pr, RESULT_ROW_START_INTERP) &&
             isResultRowInterpolated(pr, RESULT_ROW_END_INTERP));
      tdListPopHead(pResultRowInfo->openWindow);
      continue;
    }

    STimeWindow w = pr->win;
    int32_t     ret =
        setTimeWindowOutputBuf(pResultRowInfo, &w, (scanFlag == MAIN_SCAN), &pResult, groupId, pInfo->binfo.pCtx,
                               numOfOutput, pInfo->binfo.rowCellInfoOffset, &pInfo->aggSup, pTaskInfo);
    if (ret != TSDB_CODE_SUCCESS) {
      longjmp(pTaskInfo->env, TSDB_CODE_QRY_OUT_OF_MEMORY);
    }

    ASSERT(!isResultRowInterpolated(pResult, RESULT_ROW_END_INTERP));

    SGroupKeys* pTsKey = taosArrayGet(pInfo->pPrevValues, 0);
    int64_t     prevTs = *(int64_t*)pTsKey->pData;
    doTimeWindowInterpolation(pInfo, numOfOutput, pBlock->pDataBlock, prevTs, -1, tsCols[startPos], startPos, w.ekey,
                              RESULT_ROW_END_INTERP);

    setResultRowInterpo(pResult, RESULT_ROW_END_INTERP);
    setNotInterpoWindowKey(pInfo->binfo.pCtx, numOfExprs, RESULT_ROW_START_INTERP);

    doApplyFunctions(pTaskInfo, pInfo->binfo.pCtx, &w, &pInfo->twAggSup.timeWindowData, startPos, 0, tsCols,
                     pBlock->info.rows, numOfExprs, pInfo->order);

    if (isResultRowInterpolated(pResult, RESULT_ROW_END_INTERP)) {
      closeResultRow(pr);
      tdListPopHead(pResultRowInfo->openWindow);
    } else {  // the remains are can not be closed yet.
      break;
    }
  }
}

typedef int64_t (*__get_value_fn_t)(void* data, int32_t index);

int32_t binarySearch(void* keyList, int num, TSKEY key, int order, __get_value_fn_t getValuefn) {
  int firstPos = 0, lastPos = num - 1, midPos = -1;
  int numOfRows = 0;

  if (num <= 0) return -1;
  if (order == TSDB_ORDER_DESC) {
    // find the first position which is smaller or equal than the key
    while (1) {
      if (key >= getValuefn(keyList, lastPos)) return lastPos;
      if (key == getValuefn(keyList, firstPos)) return firstPos;
      if (key < getValuefn(keyList, firstPos)) return firstPos - 1;

      numOfRows = lastPos - firstPos + 1;
      midPos = (numOfRows >> 1) + firstPos;

      if (key < getValuefn(keyList, midPos)) {
        lastPos = midPos - 1;
      } else if (key > getValuefn(keyList, midPos)) {
        firstPos = midPos + 1;
      } else {
        break;
      }
    }

  } else {
    // find the first position which is bigger or equal than the key
    while (1) {
      if (key <= getValuefn(keyList, firstPos)) return firstPos;
      if (key == getValuefn(keyList, lastPos)) return lastPos;

      if (key > getValuefn(keyList, lastPos)) {
        lastPos = lastPos + 1;
        if (lastPos >= num)
          return -1;
        else
          return lastPos;
      }

      numOfRows = lastPos - firstPos + 1;
      midPos = (numOfRows >> 1) + firstPos;

      if (key < getValuefn(keyList, midPos)) {
        lastPos = midPos - 1;
      } else if (key > getValuefn(keyList, midPos)) {
        firstPos = midPos + 1;
      } else {
        break;
      }
    }
  }

  return midPos;
}

int64_t getReskey(void* data, int32_t index) {
  SArray*     res = (SArray*)data;
  SResKeyPos* pos = taosArrayGetP(res, index);
  return *(int64_t*)pos->key;
}

static int32_t saveResult(int64_t ts, int32_t pageId, int32_t offset, uint64_t groupId,
    SArray* pUpdated) {
  int32_t size = taosArrayGetSize(pUpdated);
  int32_t index = binarySearch(pUpdated, size, ts, TSDB_ORDER_DESC, getReskey);
  if (index == -1) {
    index = 0;
  } else {
    TSKEY resTs = getReskey(pUpdated, index);
    if (resTs < ts) {
      index++;
    } else {
      return TSDB_CODE_SUCCESS;
    }
  }

  SResKeyPos* newPos = taosMemoryMalloc(sizeof(SResKeyPos) + sizeof(uint64_t));
  if (newPos == NULL) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  newPos->groupId = groupId;
  newPos->pos = (SResultRowPosition){.pageId = pageId, .offset = offset};
  *(int64_t*)newPos->key = ts;
  if (taosArrayInsert(pUpdated, index, &newPos) == NULL) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t saveResultRow(SResultRow* result, uint64_t groupId, SArray* pUpdated) {
  return saveResult(result->win.skey, result->pageId, result->offset, groupId, pUpdated);
}

static void removeResult(SArray* pUpdated, TSKEY key) {
  int32_t size = taosArrayGetSize(pUpdated);
  int32_t index = binarySearch(pUpdated, size, key, TSDB_ORDER_DESC, getReskey);
  if (index >= 0 && key == getReskey(pUpdated, index)) {
    taosArrayRemove(pUpdated, index);
  }
}

static void removeResults(SArray* pWins, SArray* pUpdated) {
  int32_t size = taosArrayGetSize(pWins);
  for (int32_t i = 0; i < size; i++) {
    STimeWindow* pW = taosArrayGet(pWins, i);
    removeResult(pUpdated, pW->skey);
  }
}

static void hashIntervalAgg(SOperatorInfo* pOperatorInfo, SResultRowInfo* pResultRowInfo, SSDataBlock* pBlock,
                            int32_t scanFlag, SArray* pUpdated) {
  SIntervalAggOperatorInfo* pInfo = (SIntervalAggOperatorInfo*)pOperatorInfo->info;

  SExecTaskInfo* pTaskInfo = pOperatorInfo->pTaskInfo;

  int32_t     startPos = 0;
  int32_t     numOfOutput = pOperatorInfo->numOfExprs;
  int64_t*    tsCols = extractTsCol(pBlock, pInfo);
  uint64_t    tableGroupId = pBlock->info.groupId;
  bool        ascScan = (pInfo->order == TSDB_ORDER_ASC);
  TSKEY       ts = getStartTsKey(&pBlock->info.window, tsCols);
  SResultRow* pResult = NULL;

  STimeWindow win = getActiveTimeWindow(pInfo->aggSup.pResultBuf, pResultRowInfo, ts, &pInfo->interval,
                                        pInfo->interval.precision, &pInfo->win);

  int32_t ret =
      setTimeWindowOutputBuf(pResultRowInfo, &win, (scanFlag == MAIN_SCAN), &pResult, tableGroupId, pInfo->binfo.pCtx,
                             numOfOutput, pInfo->binfo.rowCellInfoOffset, &pInfo->aggSup, pTaskInfo);
  if (ret != TSDB_CODE_SUCCESS || pResult == NULL) {
    longjmp(pTaskInfo->env, TSDB_CODE_QRY_OUT_OF_MEMORY);
  }

  if (pInfo->execModel == OPTR_EXEC_MODEL_STREAM) {
    if (pInfo->twAggSup.calTrigger == STREAM_TRIGGER_AT_ONCE) {
      saveResultRow(pResult, tableGroupId, pUpdated);
    }
  }

  TSKEY   ekey = ascScan ? win.ekey : win.skey;
  int32_t forwardRows =
      getNumOfRowsInTimeWindow(&pBlock->info, tsCols, startPos, ekey, binarySearchForKey, NULL, pInfo->order);
  ASSERT(forwardRows > 0);

  // prev time window not interpolation yet.
  if (pInfo->timeWindowInterpo) {
    SResultRowPosition pos = addToOpenWindowList(pResultRowInfo, pResult);
    doInterpUnclosedTimeWindow(pOperatorInfo, numOfOutput, pResultRowInfo, pBlock, scanFlag, tsCols, &pos);

    // restore current time window
    ret =
        setTimeWindowOutputBuf(pResultRowInfo, &win, (scanFlag == MAIN_SCAN), &pResult, tableGroupId, pInfo->binfo.pCtx,
                               numOfOutput, pInfo->binfo.rowCellInfoOffset, &pInfo->aggSup, pTaskInfo);
    if (ret != TSDB_CODE_SUCCESS) {
      longjmp(pTaskInfo->env, TSDB_CODE_QRY_OUT_OF_MEMORY);
    }

    // window start key interpolation
    doWindowBorderInterpolation(pInfo, pBlock, numOfOutput, pInfo->binfo.pCtx, pResult, &win, startPos, forwardRows);
  }

  updateTimeWindowInfo(&pInfo->twAggSup.timeWindowData, &win, true);
  doApplyFunctions(pTaskInfo, pInfo->binfo.pCtx, &win, &pInfo->twAggSup.timeWindowData, startPos, forwardRows, tsCols,
                   pBlock->info.rows, numOfOutput, pInfo->order);

  doCloseWindow(pResultRowInfo, pInfo, pResult);

  STimeWindow nextWin = win;
  while (1) {
    int32_t prevEndPos = forwardRows - 1 + startPos;
    startPos = getNextQualifiedWindow(&pInfo->interval, &nextWin, &pBlock->info, tsCols, prevEndPos, pInfo->order);
    if (startPos < 0) {
      break;
    }

    // null data, failed to allocate more memory buffer
    int32_t code = setTimeWindowOutputBuf(pResultRowInfo, &nextWin, (scanFlag == MAIN_SCAN), &pResult, tableGroupId,
                                          pInfo->binfo.pCtx, numOfOutput, pInfo->binfo.rowCellInfoOffset,
                                          &pInfo->aggSup, pTaskInfo);
    if (code != TSDB_CODE_SUCCESS || pResult == NULL) {
      longjmp(pTaskInfo->env, TSDB_CODE_QRY_OUT_OF_MEMORY);
    }

    if (pInfo->execModel == OPTR_EXEC_MODEL_STREAM) {
      if (pInfo->twAggSup.calTrigger == STREAM_TRIGGER_AT_ONCE) {
        saveResultRow(pResult, tableGroupId, pUpdated);
      }
    }

    ekey = ascScan ? nextWin.ekey : nextWin.skey;
    forwardRows =
        getNumOfRowsInTimeWindow(&pBlock->info, tsCols, startPos, ekey, binarySearchForKey, NULL, pInfo->order);

    // window start(end) key interpolation
    doWindowBorderInterpolation(pInfo, pBlock, numOfOutput, pInfo->binfo.pCtx, pResult, &nextWin, startPos,
                                forwardRows);

    updateTimeWindowInfo(&pInfo->twAggSup.timeWindowData, &nextWin, true);
    doApplyFunctions(pTaskInfo, pInfo->binfo.pCtx, &nextWin, &pInfo->twAggSup.timeWindowData, startPos, forwardRows,
                     tsCols, pBlock->info.rows, numOfOutput, pInfo->order);
    doCloseWindow(pResultRowInfo, pInfo, pResult);
  }

  if (pInfo->timeWindowInterpo) {
    saveDataBlockLastRow(pInfo->pPrevValues, pBlock, pInfo->pInterpCols);
  }
}

void doCloseWindow(SResultRowInfo* pResultRowInfo, const SIntervalAggOperatorInfo* pInfo, SResultRow* pResult) {
  // current result is done in computing final results.
  if (pInfo->timeWindowInterpo && isResultRowInterpolated(pResult, RESULT_ROW_END_INTERP)) {
    closeResultRow(pResult);
    tdListPopHead(pResultRowInfo->openWindow);
  }
}

SResultRowPosition addToOpenWindowList(SResultRowInfo* pResultRowInfo, const SResultRow* pResult) {
  SResultRowPosition pos = (SResultRowPosition){.pageId = pResult->pageId, .offset = pResult->offset};
  SListNode*         pn = tdListGetTail(pResultRowInfo->openWindow);
  if (pn == NULL) {
    tdListAppend(pResultRowInfo->openWindow, &pos);
    return pos;
  }

  SResultRowPosition* px = (SResultRowPosition*)pn->data;
  if (px->pageId != pos.pageId || px->offset != pos.offset) {
    tdListAppend(pResultRowInfo->openWindow, &pos);
  }

  return pos;
}

int64_t* extractTsCol(SSDataBlock* pBlock, const SIntervalAggOperatorInfo* pInfo) {
  TSKEY* tsCols = NULL;
  if (pBlock->pDataBlock != NULL) {
    SColumnInfoData* pColDataInfo = taosArrayGet(pBlock->pDataBlock, pInfo->primaryTsIndex);
    tsCols = (int64_t*)pColDataInfo->pData;

    if (tsCols != NULL) {
      blockDataUpdateTsWindow(pBlock, pInfo->primaryTsIndex);
    }
  }

  return tsCols;
}

static int32_t doOpenIntervalAgg(SOperatorInfo* pOperator) {
  if (OPTR_IS_OPENED(pOperator)) {
    return TSDB_CODE_SUCCESS;
  }

  SExecTaskInfo*            pTaskInfo = pOperator->pTaskInfo;
  SIntervalAggOperatorInfo* pInfo = pOperator->info;

  int32_t scanFlag = MAIN_SCAN;

  int64_t        st = taosGetTimestampUs();
  SOperatorInfo* downstream = pOperator->pDownstream[0];

  while (1) {
    SSDataBlock* pBlock = downstream->fpSet.getNextFn(downstream);
    if (pBlock == NULL) {
      break;
    }

    getTableScanInfo(pOperator, &pInfo->order, &scanFlag);

    // the pDataBlock are always the same one, no need to call this again
    setInputDataBlock(pOperator, pInfo->binfo.pCtx, pBlock, pInfo->order, scanFlag, true);
    hashIntervalAgg(pOperator, &pInfo->binfo.resultRowInfo, pBlock, scanFlag, NULL);

#if 0  // test for encode/decode result info
    if(pOperator->fpSet.encodeResultRow){
      char *result = NULL;
      int32_t length = 0;
      SAggSupporter   *pSup = &pInfo->aggSup;
      pOperator->fpSet.encodeResultRow(pOperator, &result, &length);
      taosHashClear(pSup->pResultRowHashTable);
      pInfo->binfo.resultRowInfo.size = 0;
      pOperator->fpSet.decodeResultRow(pOperator, result);
      if(result){
        taosMemoryFree(result);
      }
    }
#endif
  }

  closeAllResultRows(&pInfo->binfo.resultRowInfo);
  initGroupedResultInfo(&pInfo->groupResInfo, pInfo->aggSup.pResultRowHashTable, pInfo->order);
  OPTR_SET_OPENED(pOperator);

  pOperator->cost.openCost = (taosGetTimestampUs() - st) / 1000.0;
  return TSDB_CODE_SUCCESS;
}

static bool compareVal(const char* v, const SStateKeys* pKey) {
  if (IS_VAR_DATA_TYPE(pKey->type)) {
    if (varDataLen(v) != varDataLen(pKey->pData)) {
      return false;
    } else {
      return strncmp(varDataVal(v), varDataVal(pKey->pData), varDataLen(v)) == 0;
    }
  } else {
    return memcmp(pKey->pData, v, pKey->bytes) == 0;
  }
}

static void doStateWindowAggImpl(SOperatorInfo* pOperator, SStateWindowOperatorInfo* pInfo, SSDataBlock* pBlock) {
  SExecTaskInfo* pTaskInfo = pOperator->pTaskInfo;

  SColumnInfoData* pStateColInfoData = taosArrayGet(pBlock->pDataBlock, pInfo->stateCol.slotId);
  int64_t          gid = pBlock->info.groupId;

  bool    masterScan = true;
  int32_t numOfOutput = pOperator->numOfExprs;
  int16_t bytes = pStateColInfoData->info.bytes;

  SColumnInfoData* pColInfoData = taosArrayGet(pBlock->pDataBlock, pInfo->tsSlotId);
  TSKEY*           tsList = (TSKEY*)pColInfoData->pData;

  SWindowRowsSup* pRowSup = &pInfo->winSup;
  pRowSup->numOfRows = 0;

  struct SColumnDataAgg* pAgg = NULL;
  for (int32_t j = 0; j < pBlock->info.rows; ++j) {
    pAgg = (pBlock->pBlockAgg != NULL) ? pBlock->pBlockAgg[pInfo->stateCol.slotId] : NULL;
    if (colDataIsNull(pStateColInfoData, pBlock->info.rows, j, pAgg)) {
      continue;
    }

    char* val = colDataGetData(pStateColInfoData, j);

    if (!pInfo->hasKey) {
      // todo extract method
      if (IS_VAR_DATA_TYPE(pInfo->stateKey.type)) {
        varDataCopy(pInfo->stateKey.pData, val);
      } else {
        memcpy(pInfo->stateKey.pData, val, bytes);
      }

      pInfo->hasKey = true;

      doKeepNewWindowStartInfo(pRowSup, tsList, j);
      doKeepTuple(pRowSup, tsList[j]);
    } else if (compareVal(val, &pInfo->stateKey)) {
      doKeepTuple(pRowSup, tsList[j]);
      if (j == 0 && pRowSup->startRowIndex != 0) {
        pRowSup->startRowIndex = 0;
      }
    } else {  // a new state window started
      SResultRow* pResult = NULL;

      // keep the time window for the closed time window.
      STimeWindow window = pRowSup->win;

      pRowSup->win.ekey = pRowSup->win.skey;
      int32_t ret =
          setTimeWindowOutputBuf(&pInfo->binfo.resultRowInfo, &window, masterScan, &pResult, gid, pInfo->binfo.pCtx,
                                 numOfOutput, pInfo->binfo.rowCellInfoOffset, &pInfo->aggSup, pTaskInfo);
      if (ret != TSDB_CODE_SUCCESS) {  // null data, too many state code
        longjmp(pTaskInfo->env, TSDB_CODE_QRY_APP_ERROR);
      }

      updateTimeWindowInfo(&pInfo->twAggSup.timeWindowData, &window, false);
      doApplyFunctions(pTaskInfo, pInfo->binfo.pCtx, &window, &pInfo->twAggSup.timeWindowData, pRowSup->startRowIndex,
                       pRowSup->numOfRows, NULL, pBlock->info.rows, numOfOutput, TSDB_ORDER_ASC);

      // here we start a new session window
      doKeepNewWindowStartInfo(pRowSup, tsList, j);
      doKeepTuple(pRowSup, tsList[j]);

      // todo extract method
      if (IS_VAR_DATA_TYPE(pInfo->stateKey.type)) {
        varDataCopy(pInfo->stateKey.pData, val);
      } else {
        memcpy(pInfo->stateKey.pData, val, bytes);
      }
    }
  }

  SResultRow* pResult = NULL;
  pRowSup->win.ekey = tsList[pBlock->info.rows - 1];
  int32_t ret =
      setTimeWindowOutputBuf(&pInfo->binfo.resultRowInfo, &pRowSup->win, masterScan, &pResult, gid, pInfo->binfo.pCtx,
                             numOfOutput, pInfo->binfo.rowCellInfoOffset, &pInfo->aggSup, pTaskInfo);
  if (ret != TSDB_CODE_SUCCESS) {  // null data, too many state code
    longjmp(pTaskInfo->env, TSDB_CODE_QRY_APP_ERROR);
  }

  updateTimeWindowInfo(&pInfo->twAggSup.timeWindowData, &pRowSup->win, false);
  doApplyFunctions(pTaskInfo, pInfo->binfo.pCtx, &pRowSup->win, &pInfo->twAggSup.timeWindowData, pRowSup->startRowIndex,
                   pRowSup->numOfRows, NULL, pBlock->info.rows, numOfOutput, TSDB_ORDER_ASC);
}

static SSDataBlock* doStateWindowAgg(SOperatorInfo* pOperator) {
  if (pOperator->status == OP_EXEC_DONE) {
    return NULL;
  }

  SStateWindowOperatorInfo* pInfo = pOperator->info;

  SExecTaskInfo*  pTaskInfo = pOperator->pTaskInfo;
  SOptrBasicInfo* pBInfo = &pInfo->binfo;

  if (pOperator->status == OP_RES_TO_RETURN) {
    doBuildResultDatablock(pOperator, pBInfo, &pInfo->groupResInfo, pInfo->aggSup.pResultBuf);
    if (pBInfo->pRes->info.rows == 0 || !hasDataInGroupInfo(&pInfo->groupResInfo)) {
      doSetOperatorCompleted(pOperator);
      return NULL;
    }

    return pBInfo->pRes;
  }

  int32_t order = TSDB_ORDER_ASC;
  int64_t st = taosGetTimestampUs();

  SOperatorInfo* downstream = pOperator->pDownstream[0];
  while (1) {
    SSDataBlock* pBlock = downstream->fpSet.getNextFn(downstream);
    if (pBlock == NULL) {
      break;
    }

    setInputDataBlock(pOperator, pBInfo->pCtx, pBlock, order, MAIN_SCAN, true);
    blockDataUpdateTsWindow(pBlock, pInfo->tsSlotId);

    doStateWindowAggImpl(pOperator, pInfo, pBlock);
  }

  pOperator->cost.openCost = (taosGetTimestampUs() - st) / 1000.0;

  pOperator->status = OP_RES_TO_RETURN;
  closeAllResultRows(&pBInfo->resultRowInfo);

  initGroupedResultInfo(&pInfo->groupResInfo, pInfo->aggSup.pResultRowHashTable, TSDB_ORDER_ASC);
  blockDataEnsureCapacity(pBInfo->pRes, pOperator->resultInfo.capacity);
  doBuildResultDatablock(pOperator, pBInfo, &pInfo->groupResInfo, pInfo->aggSup.pResultBuf);
  if (pBInfo->pRes->info.rows == 0 || !hasDataInGroupInfo(&pInfo->groupResInfo)) {
    doSetOperatorCompleted(pOperator);
  }

  size_t rows = pBInfo->pRes->info.rows;
  pOperator->resultInfo.totalRows += rows;

  return (rows == 0) ? NULL : pBInfo->pRes;
}

static SSDataBlock* doBuildIntervalResult(SOperatorInfo* pOperator) {
  SIntervalAggOperatorInfo* pInfo = pOperator->info;
  SExecTaskInfo*            pTaskInfo = pOperator->pTaskInfo;

  if (pOperator->status == OP_EXEC_DONE) {
    return NULL;
  }

  SSDataBlock* pBlock = pInfo->binfo.pRes;

  if (pInfo->execModel == OPTR_EXEC_MODEL_STREAM) {
    return pOperator->fpSet.getStreamResFn(pOperator);
  } else {
    pTaskInfo->code = pOperator->fpSet._openFn(pOperator);
    if (pTaskInfo->code != TSDB_CODE_SUCCESS) {
      return NULL;
    }

    blockDataEnsureCapacity(pBlock, pOperator->resultInfo.capacity);
    doBuildResultDatablock(pOperator, &pInfo->binfo, &pInfo->groupResInfo, pInfo->aggSup.pResultBuf);

    if (pBlock->info.rows == 0 || !hasDataInGroupInfo(&pInfo->groupResInfo)) {
      doSetOperatorCompleted(pOperator);
    }

    size_t rows = pBlock->info.rows;
    pOperator->resultInfo.totalRows += rows;

    return (rows == 0) ? NULL : pBlock;
  }
}

// todo merged with the build group result.
static void finalizeUpdatedResult(int32_t numOfOutput, SDiskbasedBuf* pBuf, SArray* pUpdateList,
                                  int32_t* rowCellInfoOffset) {
  size_t num = taosArrayGetSize(pUpdateList);

  for (int32_t i = 0; i < num; ++i) {
    SResKeyPos* pPos = taosArrayGetP(pUpdateList, i);

    SFilePage*  bufPage = getBufPage(pBuf, pPos->pos.pageId);
    SResultRow* pRow = (SResultRow*)((char*)bufPage + pPos->pos.offset);

    for (int32_t j = 0; j < numOfOutput; ++j) {
      SResultRowEntryInfo* pEntry = getResultEntryInfo(pRow, j, rowCellInfoOffset);
      if (pRow->numOfRows < pEntry->numOfRes) {
        pRow->numOfRows = pEntry->numOfRes;
      }
    }

    releaseBufPage(pBuf, bufPage);
  }
}
static void setInverFunction(SqlFunctionCtx* pCtx, int32_t num, EStreamType type) {
  for (int i = 0; i < num; i++) {
    if (type == STREAM_INVERT) {
      fmSetInvertFunc(pCtx[i].functionId, &(pCtx[i].fpSet));
    } else if (type == STREAM_NORMAL) {
      fmSetNormalFunc(pCtx[i].functionId, &(pCtx[i].fpSet));
    }
  }
}

void doClearWindowImpl(SResultRowPosition* p1, SDiskbasedBuf* pResultBuf, SOptrBasicInfo* pBinfo, int32_t numOfOutput) {
  SResultRow*     pResult = getResultRowByPos(pResultBuf, p1);
  SqlFunctionCtx* pCtx = pBinfo->pCtx;
  for (int32_t i = 0; i < numOfOutput; ++i) {
    pCtx[i].resultInfo = getResultEntryInfo(pResult, i, pBinfo->rowCellInfoOffset);
    struct SResultRowEntryInfo* pResInfo = pCtx[i].resultInfo;
    if (fmIsWindowPseudoColumnFunc(pCtx[i].functionId)) {
      continue;
    }
    pResInfo->initialized = false;
    if (pCtx[i].functionId != -1) {
      pCtx[i].fpSet.init(&pCtx[i], pResInfo);
    }
  }
}

void doClearWindow(SAggSupporter* pSup, SOptrBasicInfo* pBinfo, char* pData, int16_t bytes, uint64_t groupId,
                   int32_t numOfOutput) {
  SET_RES_WINDOW_KEY(pSup->keyBuf, pData, bytes, groupId);
  SResultRowPosition* p1 =
      (SResultRowPosition*)taosHashGet(pSup->pResultRowHashTable, pSup->keyBuf, GET_RES_WINDOW_KEY_LEN(bytes));
  if (!p1) {
    // window has been closed
    return;
  }
  doClearWindowImpl(p1, pSup->pResultBuf, pBinfo, numOfOutput);
}

static void doClearWindows(SAggSupporter* pSup, SOptrBasicInfo* pBinfo, SInterval* pInterval, int32_t tsIndex,
                           int32_t numOfOutput, SSDataBlock* pBlock, SArray* pUpWins) {
  SColumnInfoData* pColDataInfo = taosArrayGet(pBlock->pDataBlock, tsIndex);
  TSKEY*           tsCols = (TSKEY*)pColDataInfo->pData;
  int32_t          step = 0;
  for (int32_t i = 0; i < pBlock->info.rows; i += step) {
    SResultRowInfo dumyInfo;
    dumyInfo.cur.pageId = -1;
    STimeWindow win = getActiveTimeWindow(NULL, &dumyInfo, tsCols[i], pInterval, pInterval->precision, NULL);
    step = getNumOfRowsInTimeWindow(&pBlock->info, tsCols, i, win.ekey, binarySearchForKey, NULL, TSDB_ORDER_ASC);
    doClearWindow(pSup, pBinfo, (char*)&win.skey, sizeof(TKEY), pBlock->info.groupId, numOfOutput);
    if (pUpWins) {
      taosArrayPush(pUpWins, &win);
    }
  }
}

static int32_t getAllIntervalWindow(SHashObj* pHashMap, SArray* resWins) {
  void*  pIte = NULL;
  size_t keyLen = 0;
  while ((pIte = taosHashIterate(pHashMap, pIte)) != NULL) {
    void*    key = taosHashGetKey(pIte, &keyLen);
    uint64_t groupId = *(uint64_t*)key;
    ASSERT(keyLen == GET_RES_WINDOW_KEY_LEN(sizeof(TSKEY)));
    TSKEY          ts = *(int64_t*)((char*)key + sizeof(uint64_t));
    SResultRowPosition* pPos = (SResultRowPosition*)pIte;
    int32_t code = saveResult(ts, pPos->pageId, pPos->offset, groupId, resWins);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }
  }
  return TSDB_CODE_SUCCESS;
}

bool isCloseWindow(STimeWindow *pWin, STimeWindowAggSupp* pSup) {
  return pWin->ekey < pSup->maxTs - pSup->waterMark;
}

static int32_t closeIntervalWindow(SHashObj* pHashMap, STimeWindowAggSupp* pSup, SInterval* pInterval,
                                   SArray* closeWins) {
  void*  pIte = NULL;
  size_t keyLen = 0;
  while ((pIte = taosHashIterate(pHashMap, pIte)) != NULL) {
    void*    key = taosHashGetKey(pIte, &keyLen);
    uint64_t groupId = *(uint64_t*)key;
    ASSERT(keyLen == GET_RES_WINDOW_KEY_LEN(sizeof(TSKEY)));
    TSKEY          ts = *(int64_t*)((char*)key + sizeof(uint64_t));
    SResultRowInfo dumyInfo;
    dumyInfo.cur.pageId = -1;
    STimeWindow win = getActiveTimeWindow(NULL, &dumyInfo, ts, pInterval, pInterval->precision, NULL);
    if (isCloseWindow(&win, pSup)) {
      char keyBuf[GET_RES_WINDOW_KEY_LEN(sizeof(TSKEY))];
      SET_RES_WINDOW_KEY(keyBuf, &ts, sizeof(TSKEY), groupId);
      taosHashRemove(pHashMap, keyBuf, keyLen);
      SResultRowPosition* pPos = (SResultRowPosition*)pIte;
      if (pSup->calTrigger == STREAM_TRIGGER_WINDOW_CLOSE) {
        int32_t code = saveResult(ts, pPos->pageId, pPos->offset, groupId, closeWins);
        if (code != TSDB_CODE_SUCCESS) {
          return code;
        }
      }
    }
  }
  return TSDB_CODE_SUCCESS;
}

static SSDataBlock* doStreamIntervalAgg(SOperatorInfo* pOperator) {
  SIntervalAggOperatorInfo* pInfo = pOperator->info;
  SExecTaskInfo*            pTaskInfo = pOperator->pTaskInfo;

  pInfo->order = TSDB_ORDER_ASC;

  if (pOperator->status == OP_EXEC_DONE) {
    return NULL;
  }

  if (pOperator->status == OP_RES_TO_RETURN) {
    doBuildResultDatablock(pOperator, &pInfo->binfo, &pInfo->groupResInfo, pInfo->aggSup.pResultBuf);
    if (pInfo->binfo.pRes->info.rows == 0 || !hasDataInGroupInfo(&pInfo->groupResInfo)) {
      pOperator->status = OP_EXEC_DONE;
    }
    return pInfo->binfo.pRes->info.rows == 0 ? NULL : pInfo->binfo.pRes;
  }

  SOperatorInfo* downstream = pOperator->pDownstream[0];

  SArray* pUpdated = taosArrayInit(4, POINTER_BYTES);
  while (1) {
    SSDataBlock* pBlock = downstream->fpSet.getNextFn(downstream);
    if (pBlock == NULL) {
      break;
    }

    if (pBlock->info.type == STREAM_REPROCESS) {
      doClearWindows(&pInfo->aggSup, &pInfo->binfo, &pInfo->interval, 0, pOperator->numOfExprs, pBlock, NULL);
      qDebug("%s clear existed time window results for updates checked", GET_TASKID(pTaskInfo));
      continue;
    } else if (pBlock->info.type == STREAM_GET_ALL) {
      getAllIntervalWindow(pInfo->aggSup.pResultRowHashTable, pUpdated);
      continue;
    }

    // The timewindow that overlaps the timestamps of the input pBlock need to be recalculated and return to the
    // caller. Note that all the time window are not close till now.
    // the pDataBlock are always the same one, no need to call this again
    setInputDataBlock(pOperator, pInfo->binfo.pCtx, pBlock, pInfo->order, MAIN_SCAN, true);
    if (pInfo->invertible) {
      setInverFunction(pInfo->binfo.pCtx, pOperator->numOfExprs, pBlock->info.type);
    }

    pInfo->twAggSup.maxTs = TMAX(pInfo->twAggSup.maxTs, pBlock->info.window.ekey);
    hashIntervalAgg(pOperator, &pInfo->binfo.resultRowInfo, pBlock, MAIN_SCAN, pUpdated);
  }
  closeIntervalWindow(pInfo->aggSup.pResultRowHashTable, &pInfo->twAggSup, &pInfo->interval, pUpdated);

  finalizeUpdatedResult(pOperator->numOfExprs, pInfo->aggSup.pResultBuf, pUpdated, pInfo->binfo.rowCellInfoOffset);
  initMultiResInfoFromArrayList(&pInfo->groupResInfo, pUpdated);
  blockDataEnsureCapacity(pInfo->binfo.pRes, pOperator->resultInfo.capacity);
  doBuildResultDatablock(pOperator, &pInfo->binfo, &pInfo->groupResInfo, pInfo->aggSup.pResultBuf);

  pOperator->status = OP_RES_TO_RETURN;

  return pInfo->binfo.pRes->info.rows == 0 ? NULL : pInfo->binfo.pRes;
}

static void destroyStateWindowOperatorInfo(void* param, int32_t numOfOutput) {
  SStateWindowOperatorInfo* pInfo = (SStateWindowOperatorInfo*)param;
  doDestroyBasicInfo(&pInfo->binfo, numOfOutput);
  taosMemoryFreeClear(pInfo->stateKey.pData);
}

void destroyIntervalOperatorInfo(void* param, int32_t numOfOutput) {
  SIntervalAggOperatorInfo* pInfo = (SIntervalAggOperatorInfo*)param;
  doDestroyBasicInfo(&pInfo->binfo, numOfOutput);
  cleanupAggSup(&pInfo->aggSup);
}

void destroyStreamFinalIntervalOperatorInfo(void* param, int32_t numOfOutput) {
  SStreamFinalIntervalOperatorInfo* pInfo = (SStreamFinalIntervalOperatorInfo*)param;
  doDestroyBasicInfo(&pInfo->binfo, numOfOutput);
  cleanupAggSup(&pInfo->aggSup);
  if (pInfo->pChildren) {
    int32_t size = taosArrayGetSize(pInfo->pChildren);
    for (int32_t i = 0; i < size; i++) {
      SOperatorInfo* pChildOp = taosArrayGetP(pInfo->pChildren, i);
      destroyIntervalOperatorInfo(pChildOp->info, numOfOutput);
      taosMemoryFreeClear(pChildOp->info);
      taosMemoryFreeClear(pChildOp);
    }
  }
  nodesDestroyNode((SNode*)pInfo->pPhyNode);
}

static bool allInvertible(SqlFunctionCtx* pFCtx, int32_t numOfCols) {
  for (int32_t i = 0; i < numOfCols; i++) {
    if (!fmIsInvertible(pFCtx[i].functionId)) {
      return false;
    }
  }
  return true;
}

static bool timeWindowinterpNeeded(SqlFunctionCtx* pCtx, int32_t numOfCols, SIntervalAggOperatorInfo* pInfo) {
  // the primary timestamp column
  bool needed = false;
  pInfo->pInterpCols = taosArrayInit(4, sizeof(SColumn));
  pInfo->pPrevValues = taosArrayInit(4, sizeof(SGroupKeys));

  {  // ts column
    SColumn c = {0};
    c.colId = 1;
    c.slotId = pInfo->primaryTsIndex;
    c.type = TSDB_DATA_TYPE_TIMESTAMP;
    c.bytes = sizeof(int64_t);
    taosArrayPush(pInfo->pInterpCols, &c);

    SGroupKeys key = {0};
    key.bytes = c.bytes;
    key.type = c.type;
    key.isNull = true;  // to denote no value is assigned yet
    key.pData = taosMemoryCalloc(1, c.bytes);
    taosArrayPush(pInfo->pPrevValues, &key);
  }

  for (int32_t i = 0; i < numOfCols; ++i) {
    SExprInfo* pExpr = pCtx[i].pExpr;

    if (fmIsIntervalInterpoFunc(pCtx[i].functionId)) {
      SFunctParam* pParam = &pExpr->base.pParam[0];

      SColumn c = *pParam->pCol;
      taosArrayPush(pInfo->pInterpCols, &c);
      needed = true;

      SGroupKeys key = {0};
      key.bytes = c.bytes;
      key.type = c.type;
      key.isNull = false;
      key.pData = taosMemoryCalloc(1, c.bytes);
      taosArrayPush(pInfo->pPrevValues, &key);
    }
  }

  return needed;
}

void increaseTs(SqlFunctionCtx* pCtx) {
  if (pCtx[0].pExpr->pExpr->_function.pFunctNode->funcType == FUNCTION_TYPE_WSTARTTS) {
    pCtx[0].increase = true;
  }
}

SOperatorInfo* createIntervalOperatorInfo(SOperatorInfo* downstream, SExprInfo* pExprInfo, int32_t numOfCols,
                                          SSDataBlock* pResBlock, SInterval* pInterval, int32_t primaryTsSlotId,
                                          STimeWindowAggSupp* pTwAggSupp, SExecTaskInfo* pTaskInfo, bool isStream) {
  SIntervalAggOperatorInfo* pInfo = taosMemoryCalloc(1, sizeof(SIntervalAggOperatorInfo));
  SOperatorInfo*            pOperator = taosMemoryCalloc(1, sizeof(SOperatorInfo));
  if (pInfo == NULL || pOperator == NULL) {
    goto _error;
  }

  pInfo->win = pTaskInfo->window;
  pInfo->order = TSDB_ORDER_ASC;
  pInfo->interval = *pInterval;
  pInfo->execModel = pTaskInfo->execModel;
  pInfo->twAggSup = *pTwAggSupp;

  pInfo->primaryTsIndex = primaryTsSlotId;

  size_t keyBufSize = sizeof(int64_t) + sizeof(int64_t) + POINTER_BYTES;
  initResultSizeInfo(pOperator, 4096);

  int32_t code =
      initAggInfo(&pInfo->binfo, &pInfo->aggSup, pExprInfo, numOfCols, pResBlock, keyBufSize, pTaskInfo->id.str);
  
  if (isStream) {
    ASSERT(numOfCols > 0);
    increaseTs(pInfo->binfo.pCtx);
  }

  initExecTimeWindowInfo(&pInfo->twAggSup.timeWindowData, &pInfo->win);

  pInfo->invertible = allInvertible(pInfo->binfo.pCtx, numOfCols);
  pInfo->invertible = false;  // Todo(liuyao): Dependent TSDB API

  pInfo->timeWindowInterpo = timeWindowinterpNeeded(pInfo->binfo.pCtx, numOfCols, pInfo);
  if (pInfo->timeWindowInterpo) {
    pInfo->binfo.resultRowInfo.openWindow = tdListNew(sizeof(SResultRowPosition));
    if (pInfo->binfo.resultRowInfo.openWindow == NULL) {
      goto _error;
    }
  }

  initResultRowInfo(&pInfo->binfo.resultRowInfo);

  pOperator->name = "TimeIntervalAggOperator";
  pOperator->operatorType = QUERY_NODE_PHYSICAL_PLAN_HASH_INTERVAL;
  pOperator->blocking = true;
  pOperator->status = OP_NOT_OPENED;
  pOperator->pExpr = pExprInfo;
  pOperator->pTaskInfo = pTaskInfo;
  pOperator->numOfExprs = numOfCols;
  pOperator->info = pInfo;

  pOperator->fpSet = createOperatorFpSet(doOpenIntervalAgg, doBuildIntervalResult, doStreamIntervalAgg, NULL,
                                         destroyIntervalOperatorInfo, aggEncodeResultRow, aggDecodeResultRow, NULL);

  code = appendDownstream(pOperator, &downstream, 1);
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }

  return pOperator;

_error:
  destroyIntervalOperatorInfo(pInfo, numOfCols);
  taosMemoryFreeClear(pInfo);
  taosMemoryFreeClear(pOperator);
  pTaskInfo->code = code;
  return NULL;
}

SOperatorInfo* createStreamIntervalOperatorInfo(SOperatorInfo* downstream, SExprInfo* pExprInfo, int32_t numOfCols,
                                                SSDataBlock* pResBlock, SInterval* pInterval, int32_t primaryTsSlotId,
                                                STimeWindowAggSupp* pTwAggSupp, SExecTaskInfo* pTaskInfo) {
  SIntervalAggOperatorInfo* pInfo = taosMemoryCalloc(1, sizeof(SIntervalAggOperatorInfo));
  SOperatorInfo*            pOperator = taosMemoryCalloc(1, sizeof(SOperatorInfo));
  if (pInfo == NULL || pOperator == NULL) {
    goto _error;
  }

  pInfo->order = TSDB_ORDER_ASC;
  pInfo->interval = *pInterval;
  pInfo->execModel = OPTR_EXEC_MODEL_STREAM;
  pInfo->win = pTaskInfo->window;
  pInfo->twAggSup = *pTwAggSupp;
  pInfo->primaryTsIndex = primaryTsSlotId;

  int32_t numOfRows = 4096;
  size_t  keyBufSize = sizeof(int64_t) + sizeof(int64_t) + POINTER_BYTES;

  initResultSizeInfo(pOperator, numOfRows);
  int32_t code =
      initAggInfo(&pInfo->binfo, &pInfo->aggSup, pExprInfo, numOfCols, pResBlock, keyBufSize, pTaskInfo->id.str);
  initExecTimeWindowInfo(&pInfo->twAggSup.timeWindowData, &pInfo->win);

  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }

  initResultRowInfo(&pInfo->binfo.resultRowInfo);

  pOperator->name = "StreamTimeIntervalAggOperator";
  pOperator->operatorType = QUERY_NODE_PHYSICAL_PLAN_HASH_INTERVAL;
  pOperator->blocking = true;
  pOperator->status = OP_NOT_OPENED;
  pOperator->pExpr = pExprInfo;
  pOperator->pTaskInfo = pTaskInfo;
  pOperator->numOfExprs = numOfCols;
  pOperator->info = pInfo;

  pOperator->fpSet = createOperatorFpSet(doOpenIntervalAgg, doStreamIntervalAgg, doStreamIntervalAgg, NULL,
                                         destroyIntervalOperatorInfo, aggEncodeResultRow, aggDecodeResultRow, NULL);

  code = appendDownstream(pOperator, &downstream, 1);
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }

  return pOperator;

_error:
  destroyIntervalOperatorInfo(pInfo, numOfCols);
  taosMemoryFreeClear(pInfo);
  taosMemoryFreeClear(pOperator);
  pTaskInfo->code = code;
  return NULL;
}

// todo handle multiple tables cases.
static void doSessionWindowAggImpl(SOperatorInfo* pOperator, SSessionAggOperatorInfo* pInfo, SSDataBlock* pBlock) {
  SExecTaskInfo* pTaskInfo = pOperator->pTaskInfo;

  SColumnInfoData* pColInfoData = taosArrayGet(pBlock->pDataBlock, pInfo->tsSlotId);

  bool    masterScan = true;
  int32_t numOfOutput = pOperator->numOfExprs;
  int64_t gid = pBlock->info.groupId;

  int64_t gap = pInfo->gap;

  if (!pInfo->reptScan) {
    pInfo->reptScan = true;
    pInfo->winSup.prevTs = INT64_MIN;
  }

  SWindowRowsSup* pRowSup = &pInfo->winSup;
  pRowSup->numOfRows = 0;

  // In case of ascending or descending order scan data, only one time window needs to be kepted for each table.
  TSKEY* tsList = (TSKEY*)pColInfoData->pData;
  for (int32_t j = 0; j < pBlock->info.rows; ++j) {
    if (pInfo->winSup.prevTs == INT64_MIN) {
      doKeepNewWindowStartInfo(pRowSup, tsList, j);
      doKeepTuple(pRowSup, tsList[j]);
    } else if (tsList[j] - pRowSup->prevTs <= gap && (tsList[j] - pRowSup->prevTs) >= 0) {
      // The gap is less than the threshold, so it belongs to current session window that has been opened already.
      doKeepTuple(pRowSup, tsList[j]);
      if (j == 0 && pRowSup->startRowIndex != 0) {
        pRowSup->startRowIndex = 0;
      }
    } else {  // start a new session window
      SResultRow* pResult = NULL;

      // keep the time window for the closed time window.
      STimeWindow window = pRowSup->win;

      pRowSup->win.ekey = pRowSup->win.skey;
      int32_t ret =
          setTimeWindowOutputBuf(&pInfo->binfo.resultRowInfo, &window, masterScan, &pResult, gid, pInfo->binfo.pCtx,
                                 numOfOutput, pInfo->binfo.rowCellInfoOffset, &pInfo->aggSup, pTaskInfo);
      if (ret != TSDB_CODE_SUCCESS) {  // null data, too many state code
        longjmp(pTaskInfo->env, TSDB_CODE_QRY_APP_ERROR);
      }

      // pInfo->numOfRows data belong to the current session window
      updateTimeWindowInfo(&pInfo->twAggSup.timeWindowData, &window, false);
      doApplyFunctions(pTaskInfo, pInfo->binfo.pCtx, &window, &pInfo->twAggSup.timeWindowData, pRowSup->startRowIndex,
                       pRowSup->numOfRows, NULL, pBlock->info.rows, numOfOutput, TSDB_ORDER_ASC);

      // here we start a new session window
      doKeepNewWindowStartInfo(pRowSup, tsList, j);
      doKeepTuple(pRowSup, tsList[j]);
    }
  }

  SResultRow* pResult = NULL;
  pRowSup->win.ekey = tsList[pBlock->info.rows - 1];
  int32_t ret =
      setTimeWindowOutputBuf(&pInfo->binfo.resultRowInfo, &pRowSup->win, masterScan, &pResult, gid, pInfo->binfo.pCtx,
                             numOfOutput, pInfo->binfo.rowCellInfoOffset, &pInfo->aggSup, pTaskInfo);
  if (ret != TSDB_CODE_SUCCESS) {  // null data, too many state code
    longjmp(pTaskInfo->env, TSDB_CODE_QRY_APP_ERROR);
  }

  updateTimeWindowInfo(&pInfo->twAggSup.timeWindowData, &pRowSup->win, false);
  doApplyFunctions(pTaskInfo, pInfo->binfo.pCtx, &pRowSup->win, &pInfo->twAggSup.timeWindowData, pRowSup->startRowIndex,
                   pRowSup->numOfRows, NULL, pBlock->info.rows, numOfOutput, TSDB_ORDER_ASC);
}

static SSDataBlock* doSessionWindowAgg(SOperatorInfo* pOperator) {
  if (pOperator->status == OP_EXEC_DONE) {
    return NULL;
  }

  SSessionAggOperatorInfo* pInfo = pOperator->info;
  SOptrBasicInfo*          pBInfo = &pInfo->binfo;

  if (pOperator->status == OP_RES_TO_RETURN) {
    doBuildResultDatablock(pOperator, pBInfo, &pInfo->groupResInfo, pInfo->aggSup.pResultBuf);
    if (pBInfo->pRes->info.rows == 0 || !hasDataInGroupInfo(&pInfo->groupResInfo)) {
      doSetOperatorCompleted(pOperator);
      return NULL;
    }

    return pBInfo->pRes;
  }

  int64_t st = taosGetTimestampUs();
  int32_t order = TSDB_ORDER_ASC;

  SOperatorInfo* downstream = pOperator->pDownstream[0];

  while (1) {
    SSDataBlock* pBlock = downstream->fpSet.getNextFn(downstream);
    if (pBlock == NULL) {
      break;
    }

    // the pDataBlock are always the same one, no need to call this again
    setInputDataBlock(pOperator, pBInfo->pCtx, pBlock, order, MAIN_SCAN, true);
    blockDataUpdateTsWindow(pBlock, pInfo->tsSlotId);

    doSessionWindowAggImpl(pOperator, pInfo, pBlock);
  }

  pOperator->cost.openCost = (taosGetTimestampUs() - st) / 1000.0;

  // restore the value
  pOperator->status = OP_RES_TO_RETURN;
  closeAllResultRows(&pBInfo->resultRowInfo);

  initGroupedResultInfo(&pInfo->groupResInfo, pInfo->aggSup.pResultRowHashTable, TSDB_ORDER_ASC);
  blockDataEnsureCapacity(pBInfo->pRes, pOperator->resultInfo.capacity);
  doBuildResultDatablock(pOperator, pBInfo, &pInfo->groupResInfo, pInfo->aggSup.pResultBuf);
  if (pBInfo->pRes->info.rows == 0 || !hasDataInGroupInfo(&pInfo->groupResInfo)) {
    doSetOperatorCompleted(pOperator);
  }

  size_t rows = pBInfo->pRes->info.rows;
  pOperator->resultInfo.totalRows += rows;

  return (rows == 0) ? NULL : pBInfo->pRes;
}

static void doKeepPrevRows(STimeSliceOperatorInfo* pSliceInfo, const SSDataBlock* pBlock) {
  int32_t numOfCols = taosArrayGetSize(pBlock->pDataBlock);
  for(int32_t i = 0; i < numOfCols; ++i) {
    SColumnInfoData*  pColInfoData = taosArrayGet(pBlock->pDataBlock, i);

    // null data should not be kept since it can not be used to perform interpolation
    if (!colDataIsNull_s(pColInfoData, i)) {
      SGroupKeys* pkey = taosArrayGet(pSliceInfo->pPrevRow, i);

      pkey->isNull = false;
      char* val = colDataGetData(pColInfoData, i);
      memcpy(pkey->pData, val, pkey->bytes);
    }
  }
}

static SSDataBlock* doTimeslice(SOperatorInfo* pOperator) {
  if (pOperator->status == OP_EXEC_DONE) {
    return NULL;
  }

  STimeSliceOperatorInfo* pSliceInfo = pOperator->info;
  SSDataBlock* pResBlock = pSliceInfo->binfo.pRes;

//  if (pOperator->status == OP_RES_TO_RETURN) {
//    //    doBuildResultDatablock(&pRuntimeEnv->groupResInfo, pRuntimeEnv, pIntervalInfo->pRes);
//    if (pResBlock->info.rows == 0 || !hasDataInGroupInfo(&pSliceInfo->groupResInfo)) {
//      doSetOperatorCompleted(pOperator);
//    }
//
//    return pResBlock;
//  }

  int32_t order = TSDB_ORDER_ASC;
  SInterval* pInterval = &pSliceInfo->interval;
  SOperatorInfo* downstream = pOperator->pDownstream[0];

  int32_t numOfRows = 0;
  while (1) {
    SSDataBlock* pBlock = downstream->fpSet.getNextFn(downstream);
    if (pBlock == NULL) {
      break;
    }

    // the pDataBlock are always the same one, no need to call this again
    setInputDataBlock(pOperator, pSliceInfo->binfo.pCtx, pBlock, order, MAIN_SCAN, true);

    SColumnInfoData* pTsCol = taosArrayGet(pBlock->pDataBlock, 0);
    for(int32_t i = 0; i < pBlock->info.rows; ++i) {
      int64_t ts = *(int64_t*) colDataGetData(pTsCol, i);

      if (ts == pSliceInfo->current) {
        for(int32_t j = 0; j < pOperator->numOfExprs; ++j) {
          SExprInfo* pExprInfo = &pOperator->pExpr[j];
          int32_t dstSlot = pExprInfo->base.resSchema.slotId;
          int32_t srcSlot = pExprInfo->base.pParam[0].pCol->slotId;

          SColumnInfoData* pSrc = taosArrayGet(pBlock->pDataBlock, srcSlot);
          SColumnInfoData* pDst = taosArrayGet(pBlock->pDataBlock, dstSlot);

          char* v = colDataGetData(pSrc, i);
          colDataAppend(pDst, numOfRows, v, false);
        }

        numOfRows += 1;

        pSliceInfo->current += taosTimeAdd(pSliceInfo->current, pInterval->interval, pInterval->intervalUnit, pInterval->precision);
        if (pSliceInfo->current > pSliceInfo->win.ekey) {
          doSetOperatorCompleted(pOperator);
          break;
        }
      } else if (ts < pSliceInfo->current) {
        if (i != pBlock->info.window.ekey) {
          int64_t nextTs = *(int64_t*) colDataGetData(pTsCol, i + 1);
          if (nextTs > pSliceInfo->current) {
            // output the result
            for (int32_t j = 0; j < pOperator->numOfExprs; ++j) {
              SExprInfo* pExprInfo = &pOperator->pExpr[j];
              int32_t    dstSlot = pExprInfo->base.resSchema.slotId;
              int32_t    srcSlot = pExprInfo->base.pParam[0].pCol->slotId;

              SColumnInfoData* pSrc = taosArrayGet(pBlock->pDataBlock, srcSlot);
              SColumnInfoData* pDst = taosArrayGet(pBlock->pDataBlock, dstSlot);

              switch (pSliceInfo->fillType) {
                case TSDB_FILL_NULL:
                  colDataAppendNULL(pDst, numOfRows);
                  break;

                case TSDB_FILL_SET_VALUE: {
                  SVariant* pVar = &pSliceInfo->pFillColInfo[i].fillVal;

                  if (pDst->info.type == TSDB_DATA_TYPE_FLOAT) {
                    float v = 0;
                    GET_TYPED_DATA(v, float, pVar->nType, &pVar->i);
                    colDataAppend(pDst, numOfRows, (char*)&v, false);
                  } else if (pDst->info.type == TSDB_DATA_TYPE_DOUBLE) {
                    double v = 0;
                    GET_TYPED_DATA(v, double, pVar->nType, &pVar->i);
                    colDataAppend(pDst, numOfRows, (char*)&v, false);
                  } else if (IS_SIGNED_NUMERIC_TYPE(pDst->info.type)) {
                    int64_t v = 0;
                    GET_TYPED_DATA(v, int64_t, pVar->nType, &pVar->i);
                    colDataAppend(pDst, numOfRows, (char*)&v, false);
                  }
                }
                break;

                case TSDB_FILL_LINEAR:
#if 0
                if (pCtx->start.key == INT64_MIN || pCtx->start.key > pCtx->startTs
                    || pCtx->end.key == INT64_MIN || pCtx->end.key < pCtx->startTs) {
//                  goto interp_exit;
                }

              double v1 = -1, v2 = -1;
              GET_TYPED_DATA(v1, double, pCtx->inputType, &pCtx->start.val);
              GET_TYPED_DATA(v2, double, pCtx->inputType, &pCtx->end.val);

              SPoint point1 = {.key = ts, .val = &v1};
              SPoint point2 = {.key = nextTs, .val = &v2};
              SPoint point  = {.key = pCtx->startTs, .val = pCtx->pOutput};

              int32_t srcType = pCtx->inputType;
              if (isNull((char *)&pCtx->start.val, srcType) || isNull((char *)&pCtx->end.val, srcType)) {
                setNull(pCtx->pOutput, srcType, pCtx->inputBytes);
              } else {
                bool exceedMax = false, exceedMin = false;
                taosGetLinearInterpolationVal(&point, pCtx->outputType, &point1, &point2, TSDB_DATA_TYPE_DOUBLE, &exceedMax, &exceedMin);
                if (exceedMax || exceedMin) {
                  __compar_fn_t func = getComparFunc((int32_t)pCtx->inputType, 0);
                  if (func(&pCtx->start.val, &pCtx->end.val) <= 0) {
                    COPY_TYPED_DATA(pCtx->pOutput, pCtx->inputType, exceedMax ? &pCtx->start.val : &pCtx->end.val);
                  } else {
                    COPY_TYPED_DATA(pCtx->pOutput, pCtx->inputType, exceedMax ? &pCtx->end.val : &pCtx->start.val);
                  }
                }
              }
#endif
                  break;

                case TSDB_FILL_PREV: {
                  SGroupKeys* pkey = taosArrayGet(pSliceInfo->pPrevRow, srcSlot);
                  colDataAppend(pDst, numOfRows, pkey->pData, false);
                } break;

                case TSDB_FILL_NEXT: {
                } break;

                case TSDB_FILL_NONE:
                default:
                  break;
              }

              pSliceInfo->current +=
                  taosTimeAdd(pSliceInfo->current, pInterval->interval, pInterval->intervalUnit, pInterval->precision);
              if (pSliceInfo->current > pSliceInfo->win.ekey) {
                doSetOperatorCompleted(pOperator);
                break;
              }
            }
          } else {
            // ignore current row, and do nothing
          }
        } else {  // it is the last row of current block
          doKeepPrevRows(pSliceInfo, pBlock);
        }
      }
    }
  }

  // restore the value
  setTaskStatus(pOperator->pTaskInfo, TASK_COMPLETED);
  if (pResBlock->info.rows == 0) {
    pOperator->status = OP_EXEC_DONE;
  }

  return pResBlock->info.rows == 0 ? NULL : pResBlock;
}

static int32_t initTimesliceInfo(STimeSliceOperatorInfo* pInfo, SqlFunctionCtx* pCtx, int32_t numOfCols) {
  pInfo->pPrevRow = taosArrayInit(4, sizeof(SGroupKeys));
  pInfo->pCols = taosArrayInit(4, sizeof(SColumn));

  if (pInfo->pPrevRow == NULL || pInfo->pCols == NULL) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  for (int32_t i = 0; i < numOfCols; ++i) {
    SExprInfo* pExpr = pCtx[i].pExpr;

    SFunctParam* pParam = &pExpr->base.pParam[0];

    SColumn c = *pParam->pCol;
    taosArrayPush(pInfo->pCols, &c);

    SGroupKeys key = {0};
    key.bytes = c.bytes;
    key.type = c.type;
    key.isNull = false;
    key.pData = taosMemoryCalloc(1, c.bytes);
    taosArrayPush(pInfo->pPrevRow, &key);
  }

  return TSDB_CODE_SUCCESS;
}

SOperatorInfo* createTimeSliceOperatorInfo(SOperatorInfo* downstream, SExprInfo* pExprInfo, int32_t numOfCols,
                                           SSDataBlock* pResultBlock, const SNodeListNode* pValNode, SExecTaskInfo* pTaskInfo) {
  STimeSliceOperatorInfo* pInfo = taosMemoryCalloc(1, sizeof(STimeSliceOperatorInfo));
  SOperatorInfo*          pOperator = taosMemoryCalloc(1, sizeof(SOperatorInfo));
  if (pOperator == NULL || pInfo == NULL) {
    goto _error;
  }

  int32_t code = initTimesliceInfo(pInfo, pInfo->binfo.pCtx, numOfCols);
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }

  initResultRowInfo(&pInfo->binfo.resultRowInfo);
  pInfo->pFillColInfo = createFillColInfo(pExprInfo, numOfCols, pValNode);

  pInfo->binfo.pRes     = pResultBlock;

  pOperator->name       = "TimeSliceOperator";
  //  pOperator->operatorType = OP_AllTimeWindow;
  pOperator->blocking   = true;
  pOperator->status     = OP_NOT_OPENED;
  pOperator->pExpr      = pExprInfo;
  pOperator->numOfExprs = numOfCols;
  pOperator->info       = pInfo;
  pOperator->pTaskInfo  = pTaskInfo;

  pOperator->fpSet = createOperatorFpSet(operatorDummyOpenFn, doTimeslice, NULL, NULL, destroyBasicOperatorInfo,
                                         NULL, NULL, NULL);

  code = appendDownstream(pOperator, &downstream, 1);
  return pOperator;

_error:
  taosMemoryFree(pInfo);
  taosMemoryFree(pOperator);
  pTaskInfo->code = TSDB_CODE_OUT_OF_MEMORY;
  return NULL;
}

SOperatorInfo* createStatewindowOperatorInfo(SOperatorInfo* downstream, SExprInfo* pExpr, int32_t numOfCols,
                                             SSDataBlock* pResBlock, STimeWindowAggSupp* pTwAggSup, int32_t tsSlotId,
                                             SColumn* pStateKeyCol, SExecTaskInfo* pTaskInfo) {
  SStateWindowOperatorInfo* pInfo = taosMemoryCalloc(1, sizeof(SStateWindowOperatorInfo));
  SOperatorInfo*            pOperator = taosMemoryCalloc(1, sizeof(SOperatorInfo));
  if (pInfo == NULL || pOperator == NULL) {
    goto _error;
  }

  pInfo->stateCol = *pStateKeyCol;
  pInfo->stateKey.type = pInfo->stateCol.type;
  pInfo->stateKey.bytes = pInfo->stateCol.bytes;
  pInfo->stateKey.pData = taosMemoryCalloc(1, pInfo->stateCol.bytes);
  if (pInfo->stateKey.pData == NULL) {
    goto _error;
  }

  size_t keyBufSize = sizeof(int64_t) + sizeof(int64_t) + POINTER_BYTES;

  initResultSizeInfo(pOperator, 4096);
  initAggInfo(&pInfo->binfo, &pInfo->aggSup, pExpr, numOfCols, pResBlock, keyBufSize, pTaskInfo->id.str);
  initResultRowInfo(&pInfo->binfo.resultRowInfo);

  pInfo->twAggSup = *pTwAggSup;
  initExecTimeWindowInfo(&pInfo->twAggSup.timeWindowData, &pTaskInfo->window);

  pInfo->tsSlotId = tsSlotId;
  pOperator->name = "StateWindowOperator";
  pOperator->operatorType = QUERY_NODE_PHYSICAL_PLAN_MERGE_STATE;
  pOperator->blocking = true;
  pOperator->status = OP_NOT_OPENED;
  pOperator->pExpr = pExpr;
  pOperator->numOfExprs = numOfCols;
  pOperator->pTaskInfo = pTaskInfo;
  pOperator->info = pInfo;

  pOperator->fpSet = createOperatorFpSet(operatorDummyOpenFn, doStateWindowAgg, NULL, NULL,
                                         destroyStateWindowOperatorInfo, aggEncodeResultRow, aggDecodeResultRow, NULL);

  int32_t code = appendDownstream(pOperator, &downstream, 1);
  return pOperator;

_error:
  pTaskInfo->code = TSDB_CODE_SUCCESS;
  return NULL;
}

void destroySWindowOperatorInfo(void* param, int32_t numOfOutput) {
  SSessionAggOperatorInfo* pInfo = (SSessionAggOperatorInfo*)param;
  doDestroyBasicInfo(&pInfo->binfo, numOfOutput);
}

SOperatorInfo* createSessionAggOperatorInfo(SOperatorInfo* downstream, SExprInfo* pExprInfo, int32_t numOfCols,
                                            SSDataBlock* pResBlock, int64_t gap, int32_t tsSlotId,
                                            STimeWindowAggSupp* pTwAggSupp, SExecTaskInfo* pTaskInfo) {
  SSessionAggOperatorInfo* pInfo = taosMemoryCalloc(1, sizeof(SSessionAggOperatorInfo));
  SOperatorInfo*           pOperator = taosMemoryCalloc(1, sizeof(SOperatorInfo));
  if (pInfo == NULL || pOperator == NULL) {
    goto _error;
  }

  size_t keyBufSize = sizeof(int64_t) + sizeof(int64_t) + POINTER_BYTES;
  initResultSizeInfo(pOperator, 4096);

  int32_t code =
      initAggInfo(&pInfo->binfo, &pInfo->aggSup, pExprInfo, numOfCols, pResBlock, keyBufSize, pTaskInfo->id.str);
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }

  pInfo->twAggSup = *pTwAggSupp;
  initResultRowInfo(&pInfo->binfo.resultRowInfo);
  initExecTimeWindowInfo(&pInfo->twAggSup.timeWindowData, &pTaskInfo->window);

  pInfo->tsSlotId = tsSlotId;
  pInfo->gap = gap;
  pInfo->binfo.pRes = pResBlock;
  pInfo->winSup.prevTs = INT64_MIN;
  pInfo->reptScan = false;
  pOperator->name = "SessionWindowAggOperator";
  pOperator->operatorType = QUERY_NODE_PHYSICAL_PLAN_MERGE_SESSION;
  pOperator->blocking = true;
  pOperator->status = OP_NOT_OPENED;
  pOperator->pExpr = pExprInfo;
  pOperator->numOfExprs = numOfCols;
  pOperator->info = pInfo;

  pOperator->fpSet = createOperatorFpSet(operatorDummyOpenFn, doSessionWindowAgg, NULL, NULL,
                                         destroySWindowOperatorInfo, aggEncodeResultRow, aggDecodeResultRow, NULL);
  pOperator->pTaskInfo = pTaskInfo;

  code = appendDownstream(pOperator, &downstream, 1);
  return pOperator;

_error:
  if (pInfo != NULL) {
    destroySWindowOperatorInfo(pInfo, numOfCols);
  }

  taosMemoryFreeClear(pInfo);
  taosMemoryFreeClear(pOperator);
  pTaskInfo->code = code;
  return NULL;
}

bool isFinalInterval(SStreamFinalIntervalOperatorInfo* pInfo) { return pInfo->pChildren != NULL; }

void compactFunctions(SqlFunctionCtx* pDestCtx, SqlFunctionCtx* pSourceCtx, int32_t numOfOutput,
                      SExecTaskInfo* pTaskInfo) {
  for (int32_t k = 0; k < numOfOutput; ++k) {
    if (fmIsWindowPseudoColumnFunc(pDestCtx[k].functionId)) {
      continue;
    }
    int32_t code = TSDB_CODE_SUCCESS;
    if (functionNeedToExecute(&pDestCtx[k]) && pDestCtx[k].fpSet.combine != NULL) {
      code = pDestCtx[k].fpSet.combine(&pDestCtx[k], &pSourceCtx[k]);
      if (code != TSDB_CODE_SUCCESS) {
        qError("%s apply functions error, code: %s", GET_TASKID(pTaskInfo), tstrerror(code));
        pTaskInfo->code = code;
        longjmp(pTaskInfo->env, code);
      }
    }
  }
}

static void rebuildIntervalWindow(SStreamFinalIntervalOperatorInfo* pInfo, SArray* pWinArray, int32_t groupId,
                                  int32_t numOfOutput, SExecTaskInfo* pTaskInfo) {
  int32_t size = taosArrayGetSize(pWinArray);
  ASSERT(pInfo->pChildren);
  for (int32_t i = 0; i < size; i++) {
    STimeWindow* pParentWin = taosArrayGet(pWinArray, i);
    SResultRow*  pCurResult = NULL;
    setTimeWindowOutputBuf(&pInfo->binfo.resultRowInfo, pParentWin, true, &pCurResult, 0, pInfo->binfo.pCtx,
                           numOfOutput, pInfo->binfo.rowCellInfoOffset, &pInfo->aggSup, pTaskInfo);
    int32_t numOfChildren = taosArrayGetSize(pInfo->pChildren);
    for (int32_t j = 0; j < numOfChildren; j++) {
      SOperatorInfo*            pChildOp = taosArrayGetP(pInfo->pChildren, j);
      SIntervalAggOperatorInfo* pChInfo = pChildOp->info;
      SResultRow*               pChResult = NULL;
      setTimeWindowOutputBuf(&pChInfo->binfo.resultRowInfo, pParentWin, true, &pChResult, 0, pChInfo->binfo.pCtx,
                             pChildOp->numOfExprs, pChInfo->binfo.rowCellInfoOffset, &pChInfo->aggSup, pTaskInfo);
      compactFunctions(pInfo->binfo.pCtx, pChInfo->binfo.pCtx, numOfOutput, pTaskInfo);
    }
  }
}

bool isDeletedWindow(STimeWindow* pWin, uint64_t groupId, SAggSupporter* pSup) {
  SET_RES_WINDOW_KEY(pSup->keyBuf, &pWin->skey, sizeof(int64_t), groupId);
  SResultRowPosition* p1 = (SResultRowPosition*)taosHashGet(pSup->pResultRowHashTable,
      pSup->keyBuf, GET_RES_WINDOW_KEY_LEN(sizeof(int64_t)));
  return p1 == NULL;
}

static void doHashInterval(SOperatorInfo* pOperatorInfo, SSDataBlock* pSDataBlock, uint64_t tableGroupId,
                           SArray* pUpdated) {
  SStreamFinalIntervalOperatorInfo* pInfo = (SStreamFinalIntervalOperatorInfo*)pOperatorInfo->info;
  SResultRowInfo*                   pResultRowInfo = &(pInfo->binfo.resultRowInfo);
  SExecTaskInfo*                    pTaskInfo = pOperatorInfo->pTaskInfo;
  int32_t                           numOfOutput = pOperatorInfo->numOfExprs;
  int32_t                           step = 1;
  bool                              ascScan = true;
  TSKEY*                            tsCols = NULL;
  SResultRow*                       pResult = NULL;
  int32_t                           forwardRows = 0;

  if (pSDataBlock->pDataBlock != NULL) {
    SColumnInfoData* pColDataInfo = taosArrayGet(pSDataBlock->pDataBlock, pInfo->primaryTsIndex);
    tsCols = (int64_t*)pColDataInfo->pData;
  } else {
    return;
  }

  int32_t     startPos = ascScan ? 0 : (pSDataBlock->info.rows - 1);
  TSKEY       ts = getStartTsKey(&pSDataBlock->info.window, tsCols);
  STimeWindow nextWin = getActiveTimeWindow(pInfo->aggSup.pResultBuf, pResultRowInfo, ts, &pInfo->interval,
                                            pInfo->interval.precision, NULL);
  while (1) {
    if (isFinalInterval(pInfo) && isCloseWindow(&nextWin, &pInfo->twAggSup) &&
        isDeletedWindow(&nextWin, tableGroupId, &pInfo->aggSup)) {
      SArray* pUpWins = taosArrayInit(8, sizeof(STimeWindow));
      taosArrayPush(pUpWins, &nextWin);
      rebuildIntervalWindow(pInfo, pUpWins, pInfo->binfo.pRes->info.groupId,
          pOperatorInfo->numOfExprs, pOperatorInfo->pTaskInfo);
      taosArrayDestroy(pUpWins);
    }
    int32_t code = setTimeWindowOutputBuf(pResultRowInfo, &nextWin, true, &pResult, tableGroupId, pInfo->binfo.pCtx,
                                          numOfOutput, pInfo->binfo.rowCellInfoOffset, &pInfo->aggSup, pTaskInfo);
    if (code != TSDB_CODE_SUCCESS || pResult == NULL) {
      longjmp(pTaskInfo->env, TSDB_CODE_QRY_OUT_OF_MEMORY);
    }
    SResKeyPos* pos = taosMemoryMalloc(sizeof(SResKeyPos) + sizeof(uint64_t));
    pos->groupId = tableGroupId;
    pos->pos = (SResultRowPosition){.pageId = pResult->pageId, .offset = pResult->offset};
    *(int64_t*)pos->key = pResult->win.skey;
    forwardRows = getNumOfRowsInTimeWindow(&pSDataBlock->info, tsCols, startPos, nextWin.ekey, binarySearchForKey, NULL,
                                           TSDB_ORDER_ASC);
    if (pInfo->twAggSup.calTrigger == STREAM_TRIGGER_AT_ONCE && pUpdated) {
      saveResultRow(pResult, tableGroupId, pUpdated);
    }
    // window start(end) key interpolation
    // doWindowBorderInterpolation(pInfo, pSDataBlock, numOfOutput, pInfo->binfo.pCtx, pResult, &nextWin, startPos,
    // forwardRows);
    updateTimeWindowInfo(&pInfo->twAggSup.timeWindowData, &nextWin, true);
    doApplyFunctions(pTaskInfo, pInfo->binfo.pCtx, &nextWin, &pInfo->twAggSup.timeWindowData, startPos, forwardRows,
                     tsCols, pSDataBlock->info.rows, numOfOutput, TSDB_ORDER_ASC);
    int32_t prevEndPos = (forwardRows - 1) * step + startPos;
    ASSERT(pSDataBlock->info.window.skey > 0 && pSDataBlock->info.window.ekey > 0);
    startPos = getNextQualifiedWindow(&pInfo->interval, &nextWin, &pSDataBlock->info, tsCols, prevEndPos, pInfo->order);
    if (startPos < 0) {
      break;
    }
  }
}

static void clearStreamIntervalOperator(SStreamFinalIntervalOperatorInfo* pInfo) {
  taosHashClear(pInfo->aggSup.pResultRowHashTable);
  clearDiskbasedBuf(pInfo->aggSup.pResultBuf);
  cleanupResultRowInfo(&pInfo->binfo.resultRowInfo);
  initResultRowInfo(&pInfo->binfo.resultRowInfo);
}

static void clearUpdateDataBlock(SSDataBlock* pBlock) {
  if (pBlock->info.rows <= 0) {
    return;
  }
  blockDataCleanup(pBlock);
}

void copyUpdateDataBlock(SSDataBlock* pDest, SSDataBlock* pSource, int32_t tsColIndex) {
  ASSERT(pDest->info.capacity >= pSource->info.rows);
  clearUpdateDataBlock(pDest);
  SColumnInfoData* pDestCol = taosArrayGet(pDest->pDataBlock, 0);
  SColumnInfoData* pSourceCol = taosArrayGet(pSource->pDataBlock, tsColIndex);
  // copy timestamp column
  colDataAssign(pDestCol, pSourceCol, pSource->info.rows);
  for (int32_t i = 1; i < pDest->info.numOfCols; i++) {
    SColumnInfoData* pCol = taosArrayGet(pDest->pDataBlock, i);
    colDataAppendNNULL(pCol, 0, pSource->info.rows);
  }
  pDest->info.rows = pSource->info.rows;
  pDest->info.groupId = pSource->info.groupId;
  pDest->info.type = pSource->info.type;
  blockDataUpdateTsWindow(pDest, 0);
}

static int32_t getChildIndex(SSDataBlock* pBlock) {
  return pBlock->info.childId;
}

static SSDataBlock* doStreamFinalIntervalAgg(SOperatorInfo* pOperator) {
  SStreamFinalIntervalOperatorInfo* pInfo = pOperator->info;
  SOperatorInfo*                    downstream = pOperator->pDownstream[0];
  SArray*                           pUpdated = taosArrayInit(4, POINTER_BYTES);
  TSKEY                             maxTs = INT64_MIN;

  if (pOperator->status == OP_EXEC_DONE) {
    return NULL;
  } else if (pOperator->status == OP_RES_TO_RETURN) {
    doBuildResultDatablock(pOperator, &pInfo->binfo, &pInfo->groupResInfo, pInfo->aggSup.pResultBuf);
    if (pInfo->binfo.pRes->info.rows == 0) {
      pOperator->status = OP_EXEC_DONE;
      if (isFinalInterval(pInfo) || pInfo->pUpdateRes->info.rows == 0) {
        if (!isFinalInterval(pInfo)) {
          // semi interval operator clear disk buffer
          clearStreamIntervalOperator(pInfo);
        }
        return NULL;
      }
      // process the rest of the data
      pOperator->status = OP_OPENED;
      return pInfo->pUpdateRes;
    }
    return pInfo->binfo.pRes;
  }

  while (1) {
    SSDataBlock* pBlock = downstream->fpSet.getNextFn(downstream);
    if (pBlock == NULL) {
      clearUpdateDataBlock(pInfo->pUpdateRes);
      break;
    }

    if (pBlock->info.type == STREAM_REPROCESS) {
      SArray* pUpWins = taosArrayInit(8, sizeof(STimeWindow));
      doClearWindows(&pInfo->aggSup, &pInfo->binfo, &pInfo->interval, pInfo->primaryTsIndex, pOperator->numOfExprs,
                     pBlock, pUpWins);
      if (isFinalInterval(pInfo)) {
        int32_t                   childIndex = getChildIndex(pBlock);
        SOperatorInfo*            pChildOp = taosArrayGetP(pInfo->pChildren, childIndex);
        SIntervalAggOperatorInfo* pChildInfo = pChildOp->info;
        doClearWindows(&pChildInfo->aggSup, &pChildInfo->binfo, &pChildInfo->interval, pChildInfo->primaryTsIndex,
                       pChildOp->numOfExprs, pBlock, NULL);
        rebuildIntervalWindow(pInfo, pUpWins, pInfo->binfo.pRes->info.groupId, pOperator->numOfExprs,
                              pOperator->pTaskInfo);
        taosArrayDestroy(pUpWins);
        continue;
      }
      removeResults(pUpWins, pUpdated);
      copyUpdateDataBlock(pInfo->pUpdateRes, pBlock, pInfo->primaryTsIndex);
      taosArrayDestroy(pUpWins);
      break;
    } else if (pBlock->info.type == STREAM_GET_ALL && isFinalInterval(pInfo)) {
      getAllIntervalWindow(pInfo->aggSup.pResultRowHashTable, pUpdated);
      continue;
    }

    setInputDataBlock(pOperator, pInfo->binfo.pCtx, pBlock, pInfo->order, MAIN_SCAN, true);
    doHashInterval(pOperator, pBlock, pBlock->info.groupId, pUpdated);
    if (isFinalInterval(pInfo)) {
      int32_t chIndex = getChildIndex(pBlock);
      int32_t size = taosArrayGetSize(pInfo->pChildren);
      // if chIndex + 1 - size > 0, add new child
      for (int32_t i = 0; i < chIndex + 1 - size; i++) {
        SOperatorInfo* pChildOp = createStreamFinalIntervalOperatorInfo(NULL, pInfo->pPhyNode, pOperator->pTaskInfo, 0);
        if (!pChildOp) {
          longjmp(pOperator->pTaskInfo->env, TSDB_CODE_QRY_OUT_OF_MEMORY);
        }
        taosArrayPush(pInfo->pChildren, &pChildOp);
      }
      SOperatorInfo*                    pChildOp = taosArrayGetP(pInfo->pChildren, chIndex);
      SStreamFinalIntervalOperatorInfo* pChInfo = pChildOp->info;
      setInputDataBlock(pChildOp, pChInfo->binfo.pCtx, pBlock, pChInfo->order, MAIN_SCAN, true);
      doHashInterval(pChildOp, pBlock, pBlock->info.groupId, NULL);
    }
    maxTs = TMAX(maxTs, pBlock->info.window.ekey);
  }

  pInfo->twAggSup.maxTs = TMAX(pInfo->twAggSup.maxTs, maxTs);
  if (isFinalInterval(pInfo)) {
    closeIntervalWindow(pInfo->aggSup.pResultRowHashTable, &pInfo->twAggSup, &pInfo->interval, pUpdated);
  }

  finalizeUpdatedResult(pOperator->numOfExprs, pInfo->aggSup.pResultBuf, pUpdated, pInfo->binfo.rowCellInfoOffset);
  initMultiResInfoFromArrayList(&pInfo->groupResInfo, pUpdated);
  blockDataEnsureCapacity(pInfo->binfo.pRes, pOperator->resultInfo.capacity);
  doBuildResultDatablock(pOperator, &pInfo->binfo, &pInfo->groupResInfo, pInfo->aggSup.pResultBuf);
  pOperator->status = OP_RES_TO_RETURN;
  if (pInfo->binfo.pRes->info.rows == 0) {
    pOperator->status = OP_EXEC_DONE;
    if (pInfo->pUpdateRes->info.rows == 0) {
      return NULL;
    }
    // process the rest of the data
    pOperator->status = OP_OPENED;
    return pInfo->pUpdateRes;
  }
  return pInfo->binfo.pRes;
}

SOperatorInfo* createStreamFinalIntervalOperatorInfo(SOperatorInfo* downstream, SPhysiNode* pPhyNode,
                                                     SExecTaskInfo* pTaskInfo, int32_t numOfChild) {
  SIntervalPhysiNode*               pIntervalPhyNode = (SIntervalPhysiNode*)pPhyNode;
  SStreamFinalIntervalOperatorInfo* pInfo = taosMemoryCalloc(1, sizeof(SStreamFinalIntervalOperatorInfo));
  SOperatorInfo*                    pOperator = taosMemoryCalloc(1, sizeof(SOperatorInfo));
  if (pInfo == NULL || pOperator == NULL) {
    goto _error;
  }
  pInfo->order = TSDB_ORDER_ASC;
  pInfo->interval = (SInterval){.interval = pIntervalPhyNode->interval,
                                .sliding = pIntervalPhyNode->sliding,
                                .intervalUnit = pIntervalPhyNode->intervalUnit,
                                .slidingUnit = pIntervalPhyNode->slidingUnit,
                                .offset = pIntervalPhyNode->offset,
                                .precision = ((SColumnNode*)pIntervalPhyNode->window.pTspk)->node.resType.precision};
  pInfo->twAggSup = (STimeWindowAggSupp){
      .waterMark = pIntervalPhyNode->window.watermark,
      .calTrigger = pIntervalPhyNode->window.triggerType,
      .maxTs = INT64_MIN,
  };
  ASSERT(pInfo->twAggSup.calTrigger != STREAM_TRIGGER_MAX_DELAY);
  pInfo->primaryTsIndex = ((SColumnNode*)pIntervalPhyNode->window.pTspk)->slotId;
  size_t keyBufSize = sizeof(int64_t) + sizeof(int64_t) + POINTER_BYTES;
  initResultSizeInfo(pOperator, 4096);
  int32_t      numOfCols = 0;
  SExprInfo*   pExprInfo = createExprInfo(pIntervalPhyNode->window.pFuncs, NULL, &numOfCols);
  SSDataBlock* pResBlock = createResDataBlock(pPhyNode->pOutputDataBlockDesc);
  int32_t code = initAggInfo(&pInfo->binfo, &pInfo->aggSup, pExprInfo, numOfCols,
      pResBlock, keyBufSize, pTaskInfo->id.str);
  ASSERT(numOfCols > 0);
  increaseTs(pInfo->binfo.pCtx);
  initExecTimeWindowInfo(&pInfo->twAggSup.timeWindowData, &pTaskInfo->window);
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }
  initResultRowInfo(&pInfo->binfo.resultRowInfo);
  pInfo->pChildren = NULL;
  if (numOfChild > 0) {
    pInfo->pChildren = taosArrayInit(numOfChild, sizeof(SOperatorInfo));
    for (int32_t i = 0; i < numOfChild; i++) {
      SOperatorInfo* pChildOp = createStreamFinalIntervalOperatorInfo(NULL, pPhyNode, pTaskInfo, 0);
      if (pChildOp) {
        taosArrayPush(pInfo->pChildren, &pChildOp);
        continue;
      }
      goto _error;
    }
  }
  // semi interval operator does not catch result
  if (!isFinalInterval(pInfo)) {
    pInfo->twAggSup.calTrigger = STREAM_TRIGGER_AT_ONCE;
  }
  pInfo->pUpdateRes = createResDataBlock(pPhyNode->pOutputDataBlockDesc);
  pInfo->pUpdateRes->info.type = STREAM_REPROCESS;
  blockDataEnsureCapacity(pInfo->pUpdateRes, 128);
  pInfo->pPhyNode = (SPhysiNode*)nodesCloneNode((SNode*)pPhyNode);

  pOperator->name = "StreamFinalIntervalOperator";
  pOperator->operatorType = QUERY_NODE_PHYSICAL_PLAN_STREAM_FINAL_INTERVAL;
  pOperator->blocking = true;
  pOperator->status = OP_NOT_OPENED;
  pOperator->pExpr = pExprInfo;
  pOperator->pTaskInfo = pTaskInfo;
  pOperator->numOfExprs = numOfCols;
  pOperator->info = pInfo;

  pOperator->fpSet =
      createOperatorFpSet(NULL, doStreamFinalIntervalAgg, NULL, NULL, destroyStreamFinalIntervalOperatorInfo,
                          aggEncodeResultRow, aggDecodeResultRow, NULL);

  code = appendDownstream(pOperator, &downstream, 1);
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }

  return pOperator;

_error:
  destroyStreamFinalIntervalOperatorInfo(pInfo, numOfCols);
  taosMemoryFreeClear(pInfo);
  taosMemoryFreeClear(pOperator);
  pTaskInfo->code = code;
  return NULL;
}

void destroyStreamAggSupporter(SStreamAggSupporter* pSup) {
  taosMemoryFreeClear(pSup->pKeyBuf);
  void **pIte = NULL;
  while ((pIte = taosHashIterate(pSup->pResultRows, pIte)) != NULL) {
    SArray *pWins = (SArray *) (*pIte);
    taosArrayDestroy(pWins);
  }
  taosHashCleanup(pSup->pResultRows);
  destroyDiskbasedBuf(pSup->pResultBuf);
}

void destroyStreamSessionAggOperatorInfo(void* param, int32_t numOfOutput) {
  SStreamSessionAggOperatorInfo* pInfo = (SStreamSessionAggOperatorInfo*)param;
  doDestroyBasicInfo(&pInfo->binfo, numOfOutput);
  destroyStreamAggSupporter(&pInfo->streamAggSup);
  cleanupGroupResInfo(&pInfo->groupResInfo);
  if (pInfo->pChildren != NULL) {
    int32_t size = taosArrayGetSize(pInfo->pChildren);
    for (int32_t i = 0; i < size; i++) {
      SOperatorInfo*                 pChild = taosArrayGetP(pInfo->pChildren, i);
      SStreamSessionAggOperatorInfo* pChInfo = pChild->info;
      destroyStreamSessionAggOperatorInfo(pChInfo, numOfOutput);
      taosMemoryFreeClear(pChild);
      taosMemoryFreeClear(pChInfo);
    }
  }
}

int32_t initBiasicInfo(SOptrBasicInfo* pBasicInfo, SExprInfo* pExprInfo, int32_t numOfCols, SSDataBlock* pResultBlock) {
  pBasicInfo->pCtx = createSqlFunctionCtx(pExprInfo, numOfCols, &pBasicInfo->rowCellInfoOffset);
  pBasicInfo->pRes = pResultBlock;
  for (int32_t i = 0; i < numOfCols; ++i) {
    pBasicInfo->pCtx[i].pBuf = NULL;
  }
  ASSERT(numOfCols > 0);
  increaseTs(pBasicInfo->pCtx);
  return TSDB_CODE_SUCCESS;
}

void initDummyFunction(SqlFunctionCtx* pDummy, SqlFunctionCtx* pCtx, int32_t nums) {
  for (int i = 0; i < nums; i++) {
    pDummy[i].functionId = pCtx[i].functionId;
  }
}
void initDownStream(SOperatorInfo* downstream, SStreamAggSupporter* pAggSup, int64_t gap, int64_t waterMark,
                    uint8_t type) {
  ASSERT(downstream->operatorType == QUERY_NODE_PHYSICAL_PLAN_STREAM_SCAN);
  SStreamBlockScanInfo* pScanInfo = downstream->info;
  pScanInfo->sessionSup = (SessionWindowSupporter){.pStreamAggSup = pAggSup, .gap = gap, .parentType = type};
  pScanInfo->pUpdateInfo = updateInfoInit(60000, TSDB_TIME_PRECISION_MILLI, waterMark);
}

int32_t initSessionAggSupporter(SStreamAggSupporter* pSup, const char* pKey, SqlFunctionCtx* pCtx, int32_t numOfOutput) {
  return initStreamAggSupporter(pSup, pKey, pCtx, numOfOutput, sizeof(SResultWindowInfo));
}

SOperatorInfo* createStreamSessionAggOperatorInfo(SOperatorInfo* downstream, SExprInfo* pExprInfo, int32_t numOfCols,
                                                  SSDataBlock* pResBlock, int64_t gap, int32_t tsSlotId,
                                                  STimeWindowAggSupp* pTwAggSupp, SExecTaskInfo* pTaskInfo) {
  int32_t                        code = TSDB_CODE_OUT_OF_MEMORY;
  SStreamSessionAggOperatorInfo* pInfo = taosMemoryCalloc(1, sizeof(SStreamSessionAggOperatorInfo));
  SOperatorInfo*                 pOperator = taosMemoryCalloc(1, sizeof(SOperatorInfo));
  if (pInfo == NULL || pOperator == NULL) {
    goto _error;
  }

  initResultSizeInfo(pOperator, 4096);

  code = initBiasicInfo(&pInfo->binfo, pExprInfo, numOfCols, pResBlock);
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }
  
  code = initSessionAggSupporter(&pInfo->streamAggSup, "StreamSessionAggOperatorInfo", pInfo->binfo.pCtx, numOfCols);
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }

  pInfo->pDummyCtx = (SqlFunctionCtx*)taosMemoryCalloc(numOfCols, sizeof(SqlFunctionCtx));
  if (pInfo->pDummyCtx == NULL) {
    goto _error;
  }
  initDummyFunction(pInfo->pDummyCtx, pInfo->binfo.pCtx, numOfCols);

  pInfo->twAggSup = *pTwAggSupp;
  initResultRowInfo(&pInfo->binfo.resultRowInfo);
  initExecTimeWindowInfo(&pInfo->twAggSup.timeWindowData, &pTaskInfo->window);

  pInfo->primaryTsIndex = tsSlotId;
  pInfo->gap = gap;
  pInfo->binfo.pRes = pResBlock;
  pInfo->order = TSDB_ORDER_ASC;
  _hash_fn_t hashFn = taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY);
  pInfo->pStDeleted = taosHashInit(64, hashFn, true, HASH_NO_LOCK);
  pInfo->pDelIterator = NULL;
  pInfo->pDelRes = createOneDataBlock(pResBlock, false);
  blockDataEnsureCapacity(pInfo->pDelRes, 64);
  pInfo->pChildren = NULL;

  pOperator->name = "StreamSessionWindowAggOperator";
  pOperator->operatorType = QUERY_NODE_PHYSICAL_PLAN_STREAM_SESSION;
  pOperator->blocking = true;
  pOperator->status = OP_NOT_OPENED;
  pOperator->pExpr = pExprInfo;
  pOperator->numOfExprs = numOfCols;
  pOperator->info = pInfo;
  pOperator->fpSet =
      createOperatorFpSet(operatorDummyOpenFn, doStreamSessionAgg, NULL, NULL, destroyStreamSessionAggOperatorInfo,
                          aggEncodeResultRow, aggDecodeResultRow, NULL);
  pOperator->pTaskInfo = pTaskInfo;
  initDownStream(downstream, &pInfo->streamAggSup, pInfo->gap, pInfo->twAggSup.waterMark, pOperator->operatorType);
  code = appendDownstream(pOperator, &downstream, 1);
  return pOperator;

_error:
  if (pInfo != NULL) {
    destroyStreamSessionAggOperatorInfo(pInfo, numOfCols);
  }

  taosMemoryFreeClear(pInfo);
  taosMemoryFreeClear(pOperator);
  pTaskInfo->code = code;
  return NULL;
}

int64_t getSessionWindowEndkey(void* data, int32_t index) {
  SArray*            pWinInfos = (SArray*)data;
  SResultWindowInfo* pWin = taosArrayGet(pWinInfos, index);
  return pWin->win.ekey;
}
static bool isInWindow(SResultWindowInfo* pWin, TSKEY ts, int64_t gap) {
  int64_t sGap = ts - pWin->win.skey;
  int64_t eGap = pWin->win.ekey - ts;
  if ((sGap < 0 && sGap >= -gap) || (eGap < 0 && eGap >= -gap) || (sGap >= 0 && eGap >= 0)) {
    return true;
  }
  return false;
}

static SResultWindowInfo* insertNewSessionWindow(SArray* pWinInfos, TSKEY ts, int32_t index) {
  SResultWindowInfo win = {.pos.offset = -1, .pos.pageId = -1, .win.skey = ts, .win.ekey = ts, .isOutput = false};
  return taosArrayInsert(pWinInfos, index, &win);
}

static SResultWindowInfo* addNewSessionWindow(SArray* pWinInfos, TSKEY ts) {
  SResultWindowInfo win = {.pos.offset = -1, .pos.pageId = -1, .win.skey = ts, .win.ekey = ts, .isOutput = false};
  return taosArrayPush(pWinInfos, &win);
}

SArray* getWinInfos(SStreamAggSupporter* pAggSup, uint64_t groupId) {
  void** ite = taosHashGet(pAggSup->pResultRows, &groupId, sizeof(uint64_t));
  SArray* pWinInfos = NULL;
  if (ite == NULL) {
    pWinInfos = taosArrayInit(1024, pAggSup->valueSize);
    taosHashPut(pAggSup->pResultRows, &groupId, sizeof(uint64_t), &pWinInfos, sizeof(void *));
  } else {
    pWinInfos = *ite;
  }
  return pWinInfos;
}

SResultWindowInfo* getSessionTimeWindow(SStreamAggSupporter* pAggSup, TSKEY ts, uint64_t groupId, int64_t gap, int32_t* pIndex) {
  SArray* pWinInfos = getWinInfos(pAggSup, groupId);
  pAggSup->pCurWins = pWinInfos;

  int32_t size = taosArrayGetSize(pWinInfos);
  if (size == 0) {
    *pIndex = 0;
    return addNewSessionWindow(pWinInfos, ts);
  }
  // find the first position which is smaller than the key
  int32_t            index = binarySearch(pWinInfos, size, ts, TSDB_ORDER_DESC, getSessionWindowEndkey);
  SResultWindowInfo* pWin = NULL;
  if (index >= 0) {
    pWin = taosArrayGet(pWinInfos, index);
    if (isInWindow(pWin, ts, gap)) {
      *pIndex = index;
      return pWin;
    }
  }

  if (index + 1 < size) {
    pWin = taosArrayGet(pWinInfos, index + 1);
    if (isInWindow(pWin, ts, gap)) {
      *pIndex = index + 1;
      return pWin;
    }
  }

  if (index == size - 1) {
    *pIndex = taosArrayGetSize(pWinInfos);
    return addNewSessionWindow(pWinInfos, ts);
  }
  *pIndex = index + 1;
  return insertNewSessionWindow(pWinInfos, ts, index + 1);
}

int32_t updateSessionWindowInfo(SResultWindowInfo* pWinInfo, TSKEY* pTs, int32_t rows, int32_t start, int64_t gap,
                                SHashObj* pStDeleted) {
  for (int32_t i = start; i < rows; ++i) {
    if (!isInWindow(pWinInfo, pTs[i], gap)) {
      return i - start;
    }
    if (pWinInfo->win.skey > pTs[i]) {
      if (pStDeleted && pWinInfo->isOutput) {
        taosHashPut(pStDeleted, &pWinInfo->pos, sizeof(SResultRowPosition), &pWinInfo->win.skey, sizeof(TSKEY));
        pWinInfo->isOutput = false;
      }
      pWinInfo->win.skey = pTs[i];
    }
    pWinInfo->win.ekey = TMAX(pWinInfo->win.ekey, pTs[i]);
  }
  return rows - start;
}

static int32_t setWindowOutputBuf(SResultWindowInfo* pWinInfo, SResultRow** pResult, SqlFunctionCtx* pCtx,
                                  uint64_t groupId, int32_t numOfOutput, int32_t* rowCellInfoOffset,
                                  SStreamAggSupporter* pAggSup, SExecTaskInfo* pTaskInfo) {
  assert(pWinInfo->win.skey <= pWinInfo->win.ekey);
  // too many time window in query
  int32_t size = taosArrayGetSize(pAggSup->pCurWins);
  if (size > MAX_INTERVAL_TIME_WINDOW) {
    longjmp(pTaskInfo->env, TSDB_CODE_QRY_TOO_MANY_TIMEWINDOW);
  }

  if (pWinInfo->pos.pageId == -1) {
    *pResult = getNewResultRow(pAggSup->pResultBuf, groupId, pAggSup->resultRowSize);
    if (*pResult == NULL) {
      return TSDB_CODE_OUT_OF_MEMORY;
    }
    initResultRow(*pResult);

    // add a new result set for a new group
    pWinInfo->pos.pageId = (*pResult)->pageId;
    pWinInfo->pos.offset = (*pResult)->offset;
  } else {
    *pResult = getResultRowByPos(pAggSup->pResultBuf, &pWinInfo->pos);
    if (!(*pResult)) {
      qError("getResultRowByPos return NULL, TID:%s", GET_TASKID(pTaskInfo));
      return TSDB_CODE_FAILED;
    }
  }

  // set time window for current result
  (*pResult)->win = pWinInfo->win;
  setResultRowInitCtx(*pResult, pCtx, numOfOutput, rowCellInfoOffset);
  return TSDB_CODE_SUCCESS;
}

static int32_t doOneWindowAggImpl(int32_t tsColId, SOptrBasicInfo* pBinfo, SStreamAggSupporter* pAggSup,
                                  SColumnInfoData* pTimeWindowData, SSDataBlock* pSDataBlock,
                                  SResultWindowInfo* pCurWin, SResultRow** pResult, int32_t startIndex, int32_t winRows,
                                  int32_t numOutput, SExecTaskInfo* pTaskInfo) {
  SColumnInfoData* pColDataInfo = taosArrayGet(pSDataBlock->pDataBlock, tsColId);
  TSKEY*           tsCols = (int64_t*)pColDataInfo->pData;
  int32_t          code = setWindowOutputBuf(pCurWin, pResult, pBinfo->pCtx, pSDataBlock->info.groupId, numOutput,
                                             pBinfo->rowCellInfoOffset, pAggSup, pTaskInfo);
  if (code != TSDB_CODE_SUCCESS || (*pResult) == NULL) {
    return TSDB_CODE_QRY_OUT_OF_MEMORY;
  }
  updateTimeWindowInfo(pTimeWindowData, &pCurWin->win, true);
  doApplyFunctions(pTaskInfo, pBinfo->pCtx, &pCurWin->win, pTimeWindowData, startIndex, winRows, tsCols,
                   pSDataBlock->info.rows, numOutput, TSDB_ORDER_ASC);
  return TSDB_CODE_SUCCESS;
}

static int32_t doOneWindowAgg(SStreamSessionAggOperatorInfo* pInfo, SSDataBlock* pSDataBlock,
                              SResultWindowInfo* pCurWin, SResultRow** pResult, int32_t startIndex, int32_t winRows,
                              int32_t numOutput, SExecTaskInfo* pTaskInfo) {
  return doOneWindowAggImpl(pInfo->primaryTsIndex, &pInfo->binfo, &pInfo->streamAggSup, &pInfo->twAggSup.timeWindowData,
                            pSDataBlock, pCurWin, pResult, startIndex, winRows, numOutput, pTaskInfo);
}

static int32_t doOneStateWindowAgg(SStreamStateAggOperatorInfo* pInfo, SSDataBlock* pSDataBlock,
                                   SResultWindowInfo* pCurWin, SResultRow** pResult, int32_t startIndex,
                                   int32_t winRows, int32_t numOutput, SExecTaskInfo* pTaskInfo) {
  return doOneWindowAggImpl(pInfo->primaryTsIndex, &pInfo->binfo, &pInfo->streamAggSup, &pInfo->twAggSup.timeWindowData,
                            pSDataBlock, pCurWin, pResult, startIndex, winRows, numOutput, pTaskInfo);
}

int32_t getNumCompactWindow(SArray* pWinInfos, int32_t startIndex, int64_t gap) {
  SResultWindowInfo* pCurWin = taosArrayGet(pWinInfos, startIndex);
  int32_t            size = taosArrayGetSize(pWinInfos);
  // Just look for the window behind StartIndex
  for (int32_t i = startIndex + 1; i < size; i++) {
    SResultWindowInfo* pWinInfo = taosArrayGet(pWinInfos, i);
    if (!isInWindow(pCurWin, pWinInfo->win.skey, gap)) {
      return i - startIndex - 1;
    }
  }

  return size - startIndex - 1;
}

void compactTimeWindow(SStreamSessionAggOperatorInfo* pInfo, int32_t startIndex, int32_t num, uint64_t groupId,
                       int32_t numOfOutput, SExecTaskInfo* pTaskInfo, SHashObj* pStUpdated, SHashObj* pStDeleted) {
  SResultWindowInfo* pCurWin = taosArrayGet(pInfo->streamAggSup.pCurWins, startIndex);
  SResultRow*        pCurResult = NULL;
  setWindowOutputBuf(pCurWin, &pCurResult, pInfo->binfo.pCtx, groupId, numOfOutput, pInfo->binfo.rowCellInfoOffset,
                     &pInfo->streamAggSup, pTaskInfo);
  num += startIndex + 1;
  ASSERT(num <= taosArrayGetSize(pInfo->streamAggSup.pCurWins));
  // Just look for the window behind StartIndex
  for (int32_t i = startIndex + 1; i < num; i++) {
    SResultWindowInfo* pWinInfo = taosArrayGet(pInfo->streamAggSup.pCurWins, i);
    SResultRow*        pWinResult = NULL;
    setWindowOutputBuf(pWinInfo, &pWinResult, pInfo->pDummyCtx, groupId, numOfOutput, pInfo->binfo.rowCellInfoOffset,
                       &pInfo->streamAggSup, pTaskInfo);
    pCurWin->win.ekey = TMAX(pCurWin->win.ekey, pWinInfo->win.ekey);
    compactFunctions(pInfo->binfo.pCtx, pInfo->pDummyCtx, numOfOutput, pTaskInfo);
    taosHashRemove(pStUpdated, &pWinInfo->pos, sizeof(SResultRowPosition));
    if (pWinInfo->isOutput) {
      taosHashPut(pStDeleted, &pWinInfo->pos, sizeof(SResultRowPosition), &pWinInfo->win.skey, sizeof(TSKEY));
      pWinInfo->isOutput = false;
    }
    taosArrayRemove(pInfo->streamAggSup.pCurWins, i);
  }
}

typedef struct SWinRes {
  TSKEY    ts;
  uint64_t groupId;
} SWinRes;

static void doStreamSessionAggImpl(SOperatorInfo* pOperator, SSDataBlock* pSDataBlock, SHashObj* pStUpdated,
                                   SHashObj* pStDeleted) {
  SExecTaskInfo*                 pTaskInfo = pOperator->pTaskInfo;
  SStreamSessionAggOperatorInfo* pInfo = pOperator->info;
  bool                           masterScan = true;
  int32_t                        numOfOutput = pOperator->numOfExprs;
  uint64_t                       groupId = pSDataBlock->info.groupId;
  int64_t                        gap = pInfo->gap;
  int64_t                        code = TSDB_CODE_SUCCESS;

  int32_t     step = 1;
  bool        ascScan = true;
  TSKEY*      tsCols = NULL;
  SResultRow* pResult = NULL;
  int32_t     winRows = 0;

  if (pSDataBlock->pDataBlock != NULL) {
    SColumnInfoData* pColDataInfo = taosArrayGet(pSDataBlock->pDataBlock, pInfo->primaryTsIndex);
    tsCols = (int64_t*)pColDataInfo->pData;
  } else {
    return;
  }

  SStreamAggSupporter* pAggSup = &pInfo->streamAggSup;
  for (int32_t i = 0; i < pSDataBlock->info.rows;) {
    int32_t            winIndex = 0;
    SResultWindowInfo* pCurWin = getSessionTimeWindow(pAggSup, tsCols[i], groupId, gap, &winIndex);
    winRows = updateSessionWindowInfo(pCurWin, tsCols, pSDataBlock->info.rows, i, pInfo->gap, pStDeleted);
    code = doOneWindowAgg(pInfo, pSDataBlock, pCurWin, &pResult, i, winRows, numOfOutput, pTaskInfo);
    if (code != TSDB_CODE_SUCCESS || pResult == NULL) {
      longjmp(pTaskInfo->env, TSDB_CODE_QRY_OUT_OF_MEMORY);
    }
    // window start(end) key interpolation
    // doWindowBorderInterpolation(pOperatorInfo, pSDataBlock, pInfo->binfo.pCtx, pResult, &nextWin, startPos,
    // forwardRows,
    //                             pInfo->order, false);
    int32_t winNum = getNumCompactWindow(pAggSup->pCurWins, winIndex, gap);
    if (winNum > 0) {
      compactTimeWindow(pInfo, winIndex, winNum, groupId, numOfOutput, pTaskInfo, pStUpdated, pStDeleted);
    }
    pCurWin->isClosed = false;
    if (pInfo->twAggSup.calTrigger == STREAM_TRIGGER_AT_ONCE) {
      SWinRes value = {.ts = pCurWin->win.skey, .groupId = groupId};
      code = taosHashPut(pStUpdated, &pCurWin->pos, sizeof(SResultRowPosition), &value, sizeof(SWinRes));
      if (code != TSDB_CODE_SUCCESS) {
        longjmp(pTaskInfo->env, TSDB_CODE_QRY_OUT_OF_MEMORY);
      }
      pCurWin->isOutput = true;
    }
    i += winRows;
  }
}

static void doClearSessionWindows(SStreamAggSupporter* pAggSup, SOptrBasicInfo* pBinfo, SSDataBlock* pBlock,
                                  int32_t tsIndex, int32_t numOfOutput, int64_t gap, SArray* result) {
  SColumnInfoData* pColDataInfo = taosArrayGet(pBlock->pDataBlock, tsIndex);
  TSKEY*           tsCols = (TSKEY*)pColDataInfo->pData;
  int32_t          step = 0;
  for (int32_t i = 0; i < pBlock->info.rows; i += step) {
    int32_t            winIndex = 0;
    SResultWindowInfo* pCurWin = getSessionTimeWindow(pAggSup, tsCols[i], pBlock->info.groupId, gap, &winIndex);
    step = updateSessionWindowInfo(pCurWin, tsCols, pBlock->info.rows, i, gap, NULL);
    ASSERT(isInWindow(pCurWin, tsCols[i], gap));
    doClearWindowImpl(&pCurWin->pos, pAggSup->pResultBuf, pBinfo, numOfOutput);
    if (result) {
      taosArrayPush(result, pCurWin);
    }
  }
}

static int32_t copyUpdateResult(SHashObj* pStUpdated, SArray* pUpdated) {
  void*  pData = NULL;
  size_t keyLen = 0;
  while ((pData = taosHashIterate(pStUpdated, pData)) != NULL) {
    void* key = taosHashGetKey(pData, &keyLen);
    ASSERT(keyLen == sizeof(SResultRowPosition));
    SResKeyPos* pos = taosMemoryMalloc(sizeof(SResKeyPos) + sizeof(uint64_t));
    if (pos == NULL) {
      return TSDB_CODE_QRY_OUT_OF_MEMORY;
    }
    pos->groupId = ((SWinRes*)pData)->groupId;
    pos->pos = *(SResultRowPosition*)key;
    *(int64_t*)pos->key = ((SWinRes*)pData)->ts;
    taosArrayPush(pUpdated, &pos);
  }
  return TSDB_CODE_SUCCESS;
}

void doBuildDeleteDataBlock(SHashObj* pStDeleted, SSDataBlock* pBlock, void** Ite) {
  blockDataCleanup(pBlock);
  size_t keyLen = 0;
  while (((*Ite) = taosHashIterate(pStDeleted, *Ite)) != NULL) {
    SColumnInfoData* pColInfoData = taosArrayGet(pBlock->pDataBlock, 0);
    colDataAppend(pColInfoData, pBlock->info.rows, *Ite, false);
    for (int32_t i = 1; i < pBlock->info.numOfCols; i++) {
      pColInfoData = taosArrayGet(pBlock->pDataBlock, i);
      colDataAppendNULL(pColInfoData, pBlock->info.rows);
    }
    pBlock->info.rows += 1;
    if (pBlock->info.rows + 1 >= pBlock->info.capacity) {
      break;
    }
  }
  if ((*Ite) == NULL) {
    taosHashClear(pStDeleted);
  }
}

static void rebuildTimeWindow(SStreamSessionAggOperatorInfo* pInfo, SArray* pWinArray, int32_t groupId,
                              int32_t numOfOutput, SExecTaskInfo* pTaskInfo) {
  int32_t size = taosArrayGetSize(pWinArray);
  ASSERT(pInfo->pChildren);
  for (int32_t i = 0; i < size; i++) {
    SResultWindowInfo* pParentWin = taosArrayGet(pWinArray, i);
    SResultRow*        pCurResult = NULL;
    setWindowOutputBuf(pParentWin, &pCurResult, pInfo->binfo.pCtx, groupId, numOfOutput, pInfo->binfo.rowCellInfoOffset,
                       &pInfo->streamAggSup, pTaskInfo);
    int32_t numOfChildren = taosArrayGetSize(pInfo->pChildren);
    for (int32_t j = 0; j < numOfChildren; j++) {
      SOperatorInfo*                 pChild = taosArrayGetP(pInfo->pChildren, j);
      SStreamSessionAggOperatorInfo* pChInfo = pChild->info;
      SArray*                        pChWins = getWinInfos(&pChInfo->streamAggSup, groupId);
      int32_t                        chWinSize = taosArrayGetSize(pChWins);
      int32_t index = binarySearch(pChWins, chWinSize, pParentWin->win.skey, TSDB_ORDER_DESC, getSessionWindowEndkey);
      for (int32_t k = index; k > 0 && k < chWinSize; k++) {
        SResultWindowInfo* pcw = taosArrayGet(pChWins, k);
        if (pParentWin->win.skey <= pcw->win.skey && pcw->win.ekey <= pParentWin->win.ekey) {
          SResultRow* pChResult = NULL;
          setWindowOutputBuf(pcw, &pChResult, pChInfo->binfo.pCtx, groupId, numOfOutput,
                             pChInfo->binfo.rowCellInfoOffset, &pChInfo->streamAggSup, pTaskInfo);
          compactFunctions(pInfo->binfo.pCtx, pChInfo->binfo.pCtx, numOfOutput, pTaskInfo);
          continue;
        }
        break;
      }
    }
  }
}

bool isFinalSession(SStreamSessionAggOperatorInfo* pInfo) { return pInfo->pChildren != NULL; }

typedef SResultWindowInfo* (*__get_win_info_)(void*);
SResultWindowInfo* getSessionWinInfo(void* pData) { return (SResultWindowInfo*)pData; }
SResultWindowInfo* getStateWinInfo(void* pData) { return &((SStateWindowInfo*)pData)->winInfo; }

int32_t closeSessionWindow(SHashObj* pHashMap, STimeWindowAggSupp* pTwSup, SArray* pClosed,
    __get_win_info_ fn) {
  // Todo(liuyao) save window to tdb
  void **pIte = NULL;
  size_t keyLen = 0;
  while ((pIte = taosHashIterate(pHashMap, pIte)) != NULL) {
    uint64_t* pGroupId = taosHashGetKey(pIte, &keyLen);
    SArray *pWins = (SArray *) (*pIte);
    int32_t size = taosArrayGetSize(pWins);
    for (int32_t i = 0; i < size; i++) {
      void*              pWin = taosArrayGet(pWins, i);
      SResultWindowInfo* pSeWin = fn(pWin);
      if (pSeWin->win.ekey < pTwSup->maxTs - pTwSup->waterMark) {
        if (!pSeWin->isClosed) {
          pSeWin->isClosed = true;
          if (pTwSup->calTrigger == STREAM_TRIGGER_WINDOW_CLOSE) {
            int32_t code = saveResult(pSeWin->win.skey, pSeWin->pos.pageId, pSeWin->pos.offset, *pGroupId, pClosed);
            pSeWin->isOutput = true;
          }
        }
        continue;
      }
      break;
    }
  }
  return TSDB_CODE_SUCCESS;
}

int32_t getAllSessionWindow(SHashObj* pHashMap, SArray* pClosed, __get_win_info_ fn) {
  void **pIte = NULL;
  while ((pIte = taosHashIterate(pHashMap, pIte)) != NULL) {
    SArray *pWins = (SArray *) (*pIte);
    int32_t size = taosArrayGetSize(pWins);
    for (int32_t i = 0; i < size; i++) {
      void*              pWin = taosArrayGet(pWins, i);
      SResultWindowInfo* pSeWin = fn(pWin);
      if (!pSeWin->isClosed) {
        int32_t code = saveResult(pSeWin->win.skey, pSeWin->pos.pageId, pSeWin->pos.offset, 0, pClosed);
        pSeWin->isOutput = true;
      }
    }
  }
  return TSDB_CODE_SUCCESS;
}

static SSDataBlock* doStreamSessionAgg(SOperatorInfo* pOperator) {
  if (pOperator->status == OP_EXEC_DONE) {
    return NULL;
  }

  SStreamSessionAggOperatorInfo* pInfo = pOperator->info;
  SOptrBasicInfo*                pBInfo = &pInfo->binfo;
  if (pOperator->status == OP_RES_TO_RETURN) {
    doBuildDeleteDataBlock(pInfo->pStDeleted, pInfo->pDelRes, &pInfo->pDelIterator);
    if (pInfo->pDelRes->info.rows > 0) {
      return pInfo->pDelRes;
    }
    doBuildResultDatablock(pOperator, pBInfo, &pInfo->groupResInfo, pInfo->streamAggSup.pResultBuf);
    if (pBInfo->pRes->info.rows == 0 || !hasDataInGroupInfo(&pInfo->groupResInfo)) {
      doSetOperatorCompleted(pOperator);
    }
    return pBInfo->pRes->info.rows == 0 ? NULL : pBInfo->pRes;
  }

  _hash_fn_t     hashFn = taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY);
  SHashObj*      pStUpdated = taosHashInit(64, hashFn, true, HASH_NO_LOCK);
  SOperatorInfo* downstream = pOperator->pDownstream[0];
  SArray*        pUpdated = taosArrayInit(16, POINTER_BYTES);
  while (1) {
    SSDataBlock* pBlock = downstream->fpSet.getNextFn(downstream);
    if (pBlock == NULL) {
      break;
    }

    if (pBlock->info.type == STREAM_REPROCESS) {
      SArray* pWins = taosArrayInit(16, sizeof(SResultWindowInfo));
      doClearSessionWindows(&pInfo->streamAggSup, &pInfo->binfo, pBlock, 0, pOperator->numOfExprs, pInfo->gap, pWins);
      if (isFinalSession(pInfo)) {
        int32_t                        childIndex = 0;  // Todo(liuyao) get child id from SSDataBlock
        SOperatorInfo*                 pChildOp = taosArrayGetP(pInfo->pChildren, childIndex);
        SStreamSessionAggOperatorInfo* pChildInfo = pChildOp->info;
        doClearSessionWindows(&pChildInfo->streamAggSup, &pChildInfo->binfo, pBlock, 0, pChildOp->numOfExprs,
                              pChildInfo->gap, NULL);
        rebuildTimeWindow(pInfo, pWins, pBlock->info.groupId, pOperator->numOfExprs, pOperator->pTaskInfo);
      }
      taosArrayDestroy(pWins);
      continue;
    } else if (pBlock->info.type == STREAM_GET_ALL) {
      getAllSessionWindow(pInfo->streamAggSup.pResultRows, pUpdated, getSessionWinInfo);
      continue;
    }

    // the pDataBlock are always the same one, no need to call this again
    setInputDataBlock(pOperator, pBInfo->pCtx, pBlock, TSDB_ORDER_ASC, MAIN_SCAN, true);
    if (isFinalSession(pInfo)) {
      int32_t         childIndex = 0;  // Todo(liuyao) get child id from SSDataBlock
      SOptrBasicInfo* pChildOp = taosArrayGetP(pInfo->pChildren, childIndex);
      doStreamSessionAggImpl(pOperator, pBlock, NULL, NULL);
    }
    doStreamSessionAggImpl(pOperator, pBlock, pStUpdated, pInfo->pStDeleted);
    pInfo->twAggSup.maxTs = TMAX(pInfo->twAggSup.maxTs, pBlock->info.window.ekey);
  }
  // restore the value
  pOperator->status = OP_RES_TO_RETURN;

  closeSessionWindow(pInfo->streamAggSup.pResultRows, &pInfo->twAggSup, pUpdated,
                     getSessionWinInfo);
  copyUpdateResult(pStUpdated, pUpdated);
  taosHashCleanup(pStUpdated);

  finalizeUpdatedResult(pOperator->numOfExprs, pInfo->streamAggSup.pResultBuf, pUpdated,
                        pInfo->binfo.rowCellInfoOffset);
  initMultiResInfoFromArrayList(&pInfo->groupResInfo, pUpdated);
  blockDataEnsureCapacity(pInfo->binfo.pRes, pOperator->resultInfo.capacity);
  doBuildDeleteDataBlock(pInfo->pStDeleted, pInfo->pDelRes, &pInfo->pDelIterator);
  if (pInfo->pDelRes->info.rows > 0) {
    return pInfo->pDelRes;
  }
  doBuildResultDatablock(pOperator, &pInfo->binfo, &pInfo->groupResInfo, pInfo->streamAggSup.pResultBuf);
  return pBInfo->pRes->info.rows == 0 ? NULL : pBInfo->pRes;
}

SOperatorInfo* createStreamFinalSessionAggOperatorInfo(SOperatorInfo* downstream, SExprInfo* pExprInfo,
                                                       int32_t numOfCols, SSDataBlock* pResBlock, int64_t gap,
                                                       int32_t tsSlotId, STimeWindowAggSupp* pTwAggSupp,
                                                       SExecTaskInfo* pTaskInfo) {
  int32_t                        code = TSDB_CODE_OUT_OF_MEMORY;
  SStreamSessionAggOperatorInfo* pInfo = NULL;
  SOperatorInfo* pOperator = createStreamSessionAggOperatorInfo(downstream, pExprInfo, numOfCols, pResBlock, gap,
                                                                tsSlotId, pTwAggSupp, pTaskInfo);
  if (pOperator == NULL) {
    goto _error;
  }
  pOperator->name = "StreamFinalSessionWindowAggOperator";
  pOperator->operatorType = QUERY_NODE_PHYSICAL_PLAN_STREAM_FINAL_SESSION;
  int32_t numOfChild = 1;  // Todo(liuyao) get it from phy plan
  pInfo = pOperator->info;
  pInfo->pChildren = taosArrayInit(8, sizeof(void*));
  for (int32_t i = 0; i < numOfChild; i++) {
    SOperatorInfo* pChild =
        createStreamSessionAggOperatorInfo(NULL, pExprInfo, numOfCols, NULL, gap, tsSlotId, pTwAggSupp, pTaskInfo);
    if (pChild == NULL) {
      goto _error;
    }
    taosArrayPush(pInfo->pChildren, &pChild);
  }
  return pOperator;

_error:
  if (pInfo != NULL) {
    destroyStreamSessionAggOperatorInfo(pInfo, numOfCols);
  }

  taosMemoryFreeClear(pInfo);
  taosMemoryFreeClear(pOperator);
  pTaskInfo->code = code;
  return NULL;
}

void destroyStreamStateOperatorInfo(void* param, int32_t numOfOutput) {
  SStreamStateAggOperatorInfo* pInfo = (SStreamStateAggOperatorInfo*)param;
  doDestroyBasicInfo(&pInfo->binfo, numOfOutput);
  destroyStreamAggSupporter(&pInfo->streamAggSup);
  cleanupGroupResInfo(&pInfo->groupResInfo);
  if (pInfo->pChildren != NULL) {
    int32_t size = taosArrayGetSize(pInfo->pChildren);
    for (int32_t i = 0; i < size; i++) {
      SOperatorInfo*                 pChild = taosArrayGetP(pInfo->pChildren, i);
      SStreamSessionAggOperatorInfo* pChInfo = pChild->info;
      destroyStreamSessionAggOperatorInfo(pChInfo, numOfOutput);
      taosMemoryFreeClear(pChild);
      taosMemoryFreeClear(pChInfo);
    }
  }
}

int64_t getStateWinTsKey(void* data, int32_t index) {
  SStateWindowInfo* pStateWin = taosArrayGet(data, index);
  return pStateWin->winInfo.win.ekey;
}

SStateWindowInfo* addNewStateWindow(SArray* pWinInfos, TSKEY ts, char* pKeyData, SColumn* pCol) {
  SStateWindowInfo win = {
      .stateKey.bytes = pCol->bytes,
      .stateKey.type = pCol->type,
      .stateKey.pData = taosMemoryCalloc(1, pCol->bytes),
      .winInfo.pos.offset = -1,
      .winInfo.pos.pageId = -1,
      .winInfo.win.skey = ts,
      .winInfo.win.ekey = ts,
      .winInfo.isOutput = false,
      .winInfo.isClosed = false,
  };
  if (IS_VAR_DATA_TYPE(win.stateKey.type)) {
    varDataCopy(win.stateKey.pData, pKeyData);
  } else {
    memcpy(win.stateKey.pData, pKeyData, win.stateKey.bytes);
  }
  return taosArrayPush(pWinInfos, &win);
}

SStateWindowInfo* insertNewStateWindow(SArray* pWinInfos, TSKEY ts, char* pKeyData, int32_t index, SColumn* pCol) {
  SStateWindowInfo win = {
      .stateKey.bytes = pCol->bytes,
      .stateKey.type = pCol->type,
      .stateKey.pData = taosMemoryCalloc(1, pCol->bytes),
      .winInfo.pos.offset = -1,
      .winInfo.pos.pageId = -1,
      .winInfo.win.skey = ts,
      .winInfo.win.ekey = ts,
      .winInfo.isOutput = false,
      .winInfo.isClosed = false,
  };
  if (IS_VAR_DATA_TYPE(win.stateKey.type)) {
    varDataCopy(win.stateKey.pData, pKeyData);
  } else {
    memcpy(win.stateKey.pData, pKeyData, win.stateKey.bytes);
  }
  return taosArrayInsert(pWinInfos, index, &win);
}

bool isTsInWindow(SStateWindowInfo* pWin, TSKEY ts) {
  if (pWin->winInfo.win.skey <= ts && ts <= pWin->winInfo.win.ekey) {
    return true;
  }
  return false;
}

bool isEqualStateKey(SStateWindowInfo* pWin, char* pKeyData) {
  return pKeyData && compareVal(pKeyData, &pWin->stateKey);
}

SStateWindowInfo* getStateWindowByTs(SStreamAggSupporter* pAggSup, TSKEY ts, uint64_t groupId, int32_t* pIndex) {
  SArray* pWinInfos = getWinInfos(pAggSup, groupId);
  pAggSup->pCurWins = pWinInfos;
  int32_t           size = taosArrayGetSize(pWinInfos);
  int32_t           index = binarySearch(pWinInfos, size, ts, TSDB_ORDER_DESC, getStateWinTsKey);
  SStateWindowInfo* pWin = NULL;
  if (index >= 0) {
    pWin = taosArrayGet(pWinInfos, index);
    if (isTsInWindow(pWin, ts)) {
      *pIndex = index;
      return pWin;
    }
  }

  if (index + 1 < size) {
    pWin = taosArrayGet(pWinInfos, index + 1);
    if (isTsInWindow(pWin, ts)) {
      *pIndex = index + 1;
      return pWin;
    }
  }
  *pIndex = 0;
  return NULL;
}

SStateWindowInfo* getStateWindow(SStreamAggSupporter* pAggSup, TSKEY ts,
    uint64_t groupId, char* pKeyData, SColumn* pCol, int32_t* pIndex) {
  SArray* pWinInfos = getWinInfos(pAggSup, groupId);
  pAggSup->pCurWins = pWinInfos;
  int32_t size = taosArrayGetSize(pWinInfos);
  if (size == 0) {
    *pIndex = 0;
    return addNewStateWindow(pWinInfos, ts, pKeyData, pCol);
  }
  int32_t           index = binarySearch(pWinInfos, size, ts, TSDB_ORDER_DESC, getStateWinTsKey);
  SStateWindowInfo* pWin = NULL;
  if (index >= 0) {
    pWin = taosArrayGet(pWinInfos, index);
    if (isTsInWindow(pWin, ts)) {
      *pIndex = index;
      return pWin;
    }
  }

  if (index + 1 < size) {
    pWin = taosArrayGet(pWinInfos, index + 1);
    if (isTsInWindow(pWin, ts) || isEqualStateKey(pWin, pKeyData)) {
      *pIndex = index + 1;
      return pWin;
    }
  }

  if (index >= 0) {
    pWin = taosArrayGet(pWinInfos, index);
    if (isEqualStateKey(pWin, pKeyData)) {
      *pIndex = index;
      return pWin;
    }
  }

  if (index == size - 1) {
    *pIndex = taosArrayGetSize(pWinInfos);
    return addNewStateWindow(pWinInfos, ts, pKeyData, pCol);
  }
  *pIndex = index + 1;
  return insertNewStateWindow(pWinInfos, ts, pKeyData, index + 1, pCol);
}

int32_t updateStateWindowInfo(SArray* pWinInfos, int32_t winIndex, TSKEY* pTs, SColumnInfoData* pKeyCol, int32_t rows,
                              int32_t start, bool* allEqual, SHashObj* pSeDelete) {
  *allEqual = true;
  SStateWindowInfo* pWinInfo = taosArrayGet(pWinInfos, winIndex);
  for (int32_t i = start; i < rows; ++i) {
    char* pKeyData = colDataGetData(pKeyCol, i);
    if (!isTsInWindow(pWinInfo, pTs[i])) {
      if (isEqualStateKey(pWinInfo, pKeyData)) {
        int32_t size = taosArrayGetSize(pWinInfos);
        if (winIndex + 1 < size) {
          SStateWindowInfo* pNextWin = taosArrayGet(pWinInfos, winIndex + 1);
          // ts belongs to the next window
          if (pTs[i] >= pNextWin->winInfo.win.skey) {
            return i - start;
          }
        }
      } else {
        return i - start;
      }
    }
    if (pWinInfo->winInfo.win.skey > pTs[i]) {
      if (pSeDelete && pWinInfo->winInfo.isOutput) {
        taosHashPut(pSeDelete, &pWinInfo->winInfo.pos, sizeof(SResultRowPosition), &pWinInfo->winInfo.win.skey,
                    sizeof(TSKEY));
        pWinInfo->winInfo.isOutput = false;
      }
      pWinInfo->winInfo.win.skey = pTs[i];
    }
    pWinInfo->winInfo.win.ekey = TMAX(pWinInfo->winInfo.win.ekey, pTs[i]);
    if (!isEqualStateKey(pWinInfo, pKeyData)) {
      *allEqual = false;
    }
  }
  return rows - start;
}

void deleteWindow(SArray* pWinInfos, int32_t index) {
  ASSERT(index >= 0 && index < taosArrayGetSize(pWinInfos));
  taosArrayRemove(pWinInfos, index);
}

static void doClearStateWindows(SStreamAggSupporter* pAggSup, SSDataBlock* pBlock, int32_t tsIndex, SColumn* pCol,
                                int32_t keyIndex, SHashObj* pSeUpdated, SHashObj* pSeDeleted) {
  SColumnInfoData* pTsColInfo = taosArrayGet(pBlock->pDataBlock, tsIndex);
  SColumnInfoData* pKeyColInfo = taosArrayGet(pBlock->pDataBlock, keyIndex);
  TSKEY*           tsCol = (TSKEY*)pTsColInfo->pData;
  bool             allEqual = false;
  int32_t          step = 1;
  for (int32_t i = 0; i < pBlock->info.rows; i += step) {
    char*             pKeyData = colDataGetData(pKeyColInfo, i);
    int32_t           winIndex = 0;
    SStateWindowInfo* pCurWin = getStateWindowByTs(pAggSup, tsCol[i], pBlock->info.groupId, &winIndex);
    if (!pCurWin) {
      continue;
    }
    step = updateStateWindowInfo(pAggSup->pCurWins, winIndex, tsCol, pKeyColInfo, pBlock->info.rows, i, &allEqual,
                                 pSeDeleted);
    ASSERT(isTsInWindow(pCurWin, tsCol[i]) || isEqualStateKey(pCurWin, pKeyData));
    taosArrayPush(pAggSup->pScanWindow, &pCurWin->winInfo.win);
    taosHashRemove(pSeUpdated, &pCurWin->winInfo.pos, sizeof(SResultRowPosition));
    deleteWindow(pAggSup->pCurWins, winIndex);
  }
}

static void doStreamStateAggImpl(SOperatorInfo* pOperator, SSDataBlock* pSDataBlock, SHashObj* pSeUpdated,
                                 SHashObj* pStDeleted) {
  SExecTaskInfo*               pTaskInfo = pOperator->pTaskInfo;
  SStreamStateAggOperatorInfo* pInfo = pOperator->info;
  bool                         masterScan = true;
  int32_t                      numOfOutput = pOperator->numOfExprs;
  int64_t                      groupId = pSDataBlock->info.groupId;
  int64_t                      code = TSDB_CODE_SUCCESS;
  int32_t                      step = 1;
  bool                         ascScan = true;
  TSKEY*                       tsCols = NULL;
  SResultRow*                  pResult = NULL;
  int32_t                      winRows = 0;
  if (pSDataBlock->pDataBlock != NULL) {
    SColumnInfoData* pColDataInfo = taosArrayGet(pSDataBlock->pDataBlock, pInfo->primaryTsIndex);
    tsCols = (int64_t*)pColDataInfo->pData;
  } else {
    return;
  }

  SStreamAggSupporter* pAggSup = &pInfo->streamAggSup;
  SColumnInfoData*     pKeyColInfo = taosArrayGet(pSDataBlock->pDataBlock, pInfo->stateCol.slotId);
  for (int32_t i = 0; i < pSDataBlock->info.rows; i += winRows) {
    char*             pKeyData = colDataGetData(pKeyColInfo, i);
    int32_t           winIndex = 0;
    bool              allEqual = true;
    SStateWindowInfo* pCurWin = 
        getStateWindow(pAggSup, tsCols[i], pSDataBlock->info.groupId, pKeyData,
            &pInfo->stateCol, &winIndex);
    winRows = updateStateWindowInfo(pAggSup->pCurWins, winIndex, tsCols, pKeyColInfo,
        pSDataBlock->info.rows, i, &allEqual, pInfo->pSeDeleted);
    if (!allEqual) {
      taosArrayPush(pAggSup->pScanWindow, &pCurWin->winInfo.win);
      taosHashRemove(pSeUpdated, &pCurWin->winInfo.pos, sizeof(SResultRowPosition));
      deleteWindow(pAggSup->pCurWins, winIndex);
      continue;
    }
    code = doOneStateWindowAgg(pInfo, pSDataBlock, &pCurWin->winInfo, &pResult, i, winRows, numOfOutput, pTaskInfo);
    if (code != TSDB_CODE_SUCCESS || pResult == NULL) {
      longjmp(pTaskInfo->env, TSDB_CODE_QRY_OUT_OF_MEMORY);
    }
    pCurWin->winInfo.isClosed = false;
    if (pInfo->twAggSup.calTrigger == STREAM_TRIGGER_AT_ONCE) {
      SWinRes value = {.ts = pCurWin->winInfo.win.skey, .groupId = groupId};
      code = taosHashPut(pSeUpdated, &pCurWin->winInfo.pos, sizeof(SResultRowPosition),
          &value, sizeof(SWinRes));
      if (code != TSDB_CODE_SUCCESS) {
        longjmp(pTaskInfo->env, TSDB_CODE_QRY_OUT_OF_MEMORY);
      }
      pCurWin->winInfo.isOutput = true;
    }
  }
}

static SSDataBlock* doStreamStateAgg(SOperatorInfo* pOperator) {
  if (pOperator->status == OP_EXEC_DONE) {
    return NULL;
  }

  SStreamStateAggOperatorInfo* pInfo = pOperator->info;
  SOptrBasicInfo*              pBInfo = &pInfo->binfo;
  if (pOperator->status == OP_RES_TO_RETURN) {
    doBuildDeleteDataBlock(pInfo->pSeDeleted, pInfo->pDelRes, &pInfo->pDelIterator);
    if (pInfo->pDelRes->info.rows > 0) {
      return pInfo->pDelRes;
    }
    doBuildResultDatablock(pOperator, pBInfo, &pInfo->groupResInfo, pInfo->streamAggSup.pResultBuf);
    if (pBInfo->pRes->info.rows == 0 || !hasDataInGroupInfo(&pInfo->groupResInfo)) {
      doSetOperatorCompleted(pOperator);
    }
    return pBInfo->pRes->info.rows == 0 ? NULL : pBInfo->pRes;
  }

  _hash_fn_t     hashFn = taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY);
  SHashObj*      pSeUpdated = taosHashInit(64, hashFn, true, HASH_NO_LOCK);
  SOperatorInfo* downstream = pOperator->pDownstream[0];
  SArray*        pUpdated = taosArrayInit(16, POINTER_BYTES);
  while (1) {
    SSDataBlock* pBlock = downstream->fpSet.getNextFn(downstream);
    if (pBlock == NULL) {
      break;
    }

    if (pBlock->info.type == STREAM_REPROCESS) {
      doClearStateWindows(&pInfo->streamAggSup, pBlock, pInfo->primaryTsIndex, &pInfo->stateCol, pInfo->stateCol.slotId,
                          pSeUpdated, pInfo->pSeDeleted);
      continue;
    } else if (pBlock->info.type == STREAM_GET_ALL) {
      getAllSessionWindow(pInfo->streamAggSup.pResultRows, pUpdated, getStateWinInfo);
      continue;
    }

    // the pDataBlock are always the same one, no need to call this again
    setInputDataBlock(pOperator, pBInfo->pCtx, pBlock, TSDB_ORDER_ASC, MAIN_SCAN, true);
    doStreamStateAggImpl(pOperator, pBlock, pSeUpdated, pInfo->pSeDeleted);
    pInfo->twAggSup.maxTs = TMAX(pInfo->twAggSup.maxTs, pBlock->info.window.ekey);
  }
  // restore the value
  pOperator->status = OP_RES_TO_RETURN;

  closeSessionWindow(pInfo->streamAggSup.pResultRows, &pInfo->twAggSup, pUpdated,
                     getStateWinInfo);
  copyUpdateResult(pSeUpdated, pUpdated);
  taosHashCleanup(pSeUpdated);

  finalizeUpdatedResult(pOperator->numOfExprs, pInfo->streamAggSup.pResultBuf, pUpdated,
                        pInfo->binfo.rowCellInfoOffset);
  initMultiResInfoFromArrayList(&pInfo->groupResInfo, pUpdated);
  blockDataEnsureCapacity(pInfo->binfo.pRes, pOperator->resultInfo.capacity);
  doBuildDeleteDataBlock(pInfo->pSeDeleted, pInfo->pDelRes, &pInfo->pDelIterator);
  if (pInfo->pDelRes->info.rows > 0) {
    return pInfo->pDelRes;
  }
  doBuildResultDatablock(pOperator, &pInfo->binfo, &pInfo->groupResInfo, pInfo->streamAggSup.pResultBuf);
  return pBInfo->pRes->info.rows == 0 ? NULL : pBInfo->pRes;
}

int32_t initStateAggSupporter(SStreamAggSupporter* pSup, const char* pKey, SqlFunctionCtx* pCtx, int32_t numOfOutput) {
  return initStreamAggSupporter(pSup, pKey, pCtx, numOfOutput, sizeof(SStateWindowInfo));
}

SOperatorInfo* createStreamStateAggOperatorInfo(SOperatorInfo* downstream, SPhysiNode* pPhyNode,
                                                SExecTaskInfo* pTaskInfo) {
  SStreamStateWinodwPhysiNode* pStateNode = (SStreamStateWinodwPhysiNode*)pPhyNode;
  SSDataBlock*                 pResBlock = createResDataBlock(pPhyNode->pOutputDataBlockDesc);
  int32_t                      tsSlotId = ((SColumnNode*)pStateNode->window.pTspk)->slotId;
  SColumnNode*                 pColNode = (SColumnNode*)((STargetNode*)pStateNode->pStateKey)->pExpr;
  int32_t                      code = TSDB_CODE_OUT_OF_MEMORY;

  SStreamStateAggOperatorInfo* pInfo = taosMemoryCalloc(1, sizeof(SStreamStateAggOperatorInfo));
  SOperatorInfo*               pOperator = taosMemoryCalloc(1, sizeof(SOperatorInfo));
  if (pInfo == NULL || pOperator == NULL) {
    goto _error;
  }

  int32_t    numOfCols = 0;
  SExprInfo* pExprInfo = createExprInfo(pStateNode->window.pFuncs, NULL, &numOfCols);

  pInfo->stateCol = extractColumnFromColumnNode(pColNode);
  initResultSizeInfo(pOperator, 4096);
  initResultRowInfo(&pInfo->binfo.resultRowInfo);
  pInfo->twAggSup = (STimeWindowAggSupp){
      .waterMark = pStateNode->window.watermark,
      .calTrigger = pStateNode->window.triggerType,
      .maxTs = INT64_MIN,
  };
  initExecTimeWindowInfo(&pInfo->twAggSup.timeWindowData, &pTaskInfo->window);

  code = initBiasicInfo(&pInfo->binfo, pExprInfo, numOfCols, pResBlock);
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }

  code = initStateAggSupporter(&pInfo->streamAggSup, "StreamStateAggOperatorInfo", pInfo->binfo.pCtx, numOfCols);
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }

  pInfo->pDummyCtx = (SqlFunctionCtx*)taosMemoryCalloc(numOfCols, sizeof(SqlFunctionCtx));
  if (pInfo->pDummyCtx == NULL) {
    goto _error;
  }

  initDummyFunction(pInfo->pDummyCtx, pInfo->binfo.pCtx, numOfCols);
  pInfo->primaryTsIndex = tsSlotId;
  pInfo->order = TSDB_ORDER_ASC;
  _hash_fn_t hashFn = taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY);
  pInfo->pSeDeleted = taosHashInit(64, hashFn, true, HASH_NO_LOCK);
  pInfo->pDelIterator = NULL;
  pInfo->pDelRes = createOneDataBlock(pResBlock, false);
  blockDataEnsureCapacity(pInfo->pDelRes, 64);
  pInfo->pChildren = NULL;

  pOperator->name = "StreamStateAggOperator";
  pOperator->operatorType = QUERY_NODE_PHYSICAL_PLAN_STREAM_STATE;
  pOperator->blocking = true;
  pOperator->status = OP_NOT_OPENED;
  pOperator->numOfExprs = numOfCols;
  pOperator->pExpr = pExprInfo;
  pOperator->pTaskInfo = pTaskInfo;
  pOperator->info = pInfo;
  pOperator->fpSet = createOperatorFpSet(operatorDummyOpenFn, doStreamStateAgg, NULL, NULL,
                                         destroyStreamStateOperatorInfo, aggEncodeResultRow, aggDecodeResultRow, NULL);
  initDownStream(downstream, &pInfo->streamAggSup, 0, pInfo->twAggSup.waterMark, pOperator->operatorType);
  code = appendDownstream(pOperator, &downstream, 1);
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }
  return pOperator;

_error:
  destroyStreamStateOperatorInfo(pInfo, numOfCols);
  taosMemoryFreeClear(pInfo);
  taosMemoryFreeClear(pOperator);
  pTaskInfo->code = code;
  return NULL;
}

typedef struct SMergeIntervalAggOperatorInfo {
  SIntervalAggOperatorInfo intervalAggOperatorInfo;

  bool         hasGroupId;
  uint64_t     groupId;
  SSDataBlock* prefetchedBlock;
  bool         inputBlocksFinished;
} SMergeIntervalAggOperatorInfo;

void destroyMergeIntervalOperatorInfo(void* param, int32_t numOfOutput) {
  SMergeIntervalAggOperatorInfo* miaInfo = (SMergeIntervalAggOperatorInfo*)param;
  destroyIntervalOperatorInfo(&miaInfo->intervalAggOperatorInfo, numOfOutput);
}

static int32_t outputMergeIntervalResult(SOperatorInfo* pOperatorInfo, uint64_t tableGroupId, SSDataBlock* pResultBlock, TSKEY wstartTs) {
  SMergeIntervalAggOperatorInfo* miaInfo = pOperatorInfo->info;
  SIntervalAggOperatorInfo*      iaInfo = &miaInfo->intervalAggOperatorInfo;
  SExecTaskInfo*                 pTaskInfo = pOperatorInfo->pTaskInfo;
  bool                           ascScan = (iaInfo->order == TSDB_ORDER_ASC);

  SET_RES_WINDOW_KEY(iaInfo->aggSup.keyBuf, &wstartTs, TSDB_KEYSIZE, tableGroupId);
  SResultRowPosition* p1 = (SResultRowPosition*)taosHashGet(iaInfo->aggSup.pResultRowHashTable, iaInfo->aggSup.keyBuf,
                                                            GET_RES_WINDOW_KEY_LEN(TSDB_KEYSIZE));
  ASSERT(p1 != NULL);

  finalizeResultRowIntoResultDataBlock(iaInfo->aggSup.pResultBuf, p1, iaInfo->binfo.pCtx, pOperatorInfo->pExpr,
                                       pOperatorInfo->numOfExprs, iaInfo->binfo.rowCellInfoOffset, pResultBlock,
                                       pTaskInfo);
  taosHashRemove(iaInfo->aggSup.pResultRowHashTable, iaInfo->aggSup.keyBuf, GET_RES_WINDOW_KEY_LEN(TSDB_KEYSIZE));

  return 0;
}

static void doMergeIntervalAggImpl(SOperatorInfo* pOperatorInfo, SResultRowInfo* pResultRowInfo, SSDataBlock* pBlock,
                                   int32_t scanFlag, SSDataBlock* pResultBlock) {
  SMergeIntervalAggOperatorInfo* miaInfo = pOperatorInfo->info;
  SIntervalAggOperatorInfo*      iaInfo = &miaInfo->intervalAggOperatorInfo;

  SExecTaskInfo* pTaskInfo = pOperatorInfo->pTaskInfo;

  int32_t     startPos = 0;
  int32_t     numOfOutput = pOperatorInfo->numOfExprs;
  int64_t*    tsCols = extractTsCol(pBlock, iaInfo);
  uint64_t    tableGroupId = pBlock->info.groupId;
  TSKEY       blockStartTs = getStartTsKey(&pBlock->info.window, tsCols);
  SResultRow* pResult = NULL;

  STimeWindow win;
  win.skey = blockStartTs;
  win.ekey = taosTimeAdd(win.skey, iaInfo->interval.interval, iaInfo->interval.intervalUnit, iaInfo->interval.precision) - 1;

  //TODO: remove the hash table usage (groupid + winkey => result row position)
  int32_t ret =
      setTimeWindowOutputBuf(pResultRowInfo, &win, (scanFlag == MAIN_SCAN), &pResult, tableGroupId, iaInfo->binfo.pCtx,
                             numOfOutput, iaInfo->binfo.rowCellInfoOffset, &iaInfo->aggSup, pTaskInfo);
  if (ret != TSDB_CODE_SUCCESS || pResult == NULL) {
    longjmp(pTaskInfo->env, TSDB_CODE_QRY_OUT_OF_MEMORY);
  }

  TSKEY currTs = blockStartTs;
  TSKEY currPos = startPos;
  STimeWindow currWin = win;
  while (1) {
    ++currPos;
    if (currPos >= pBlock->info.rows) {
      break;
    }
    if (tsCols[currPos] == currTs) {
      continue;
    } else {
      updateTimeWindowInfo(&iaInfo->twAggSup.timeWindowData, &currWin, true);
      doApplyFunctions(pTaskInfo, iaInfo->binfo.pCtx, &currWin, &iaInfo->twAggSup.timeWindowData, startPos,
                       currPos - startPos, tsCols, pBlock->info.rows, numOfOutput, iaInfo->order);

      outputMergeIntervalResult(pOperatorInfo, tableGroupId, pResultBlock, currTs);

      currTs = tsCols[currPos];
      currWin.skey = currTs;
      currWin.ekey = taosTimeAdd(currWin.skey, iaInfo->interval.interval, iaInfo->interval.intervalUnit, iaInfo->interval.precision) - 1;
      startPos = currPos;
      ret = setTimeWindowOutputBuf(pResultRowInfo, &currWin, (scanFlag == MAIN_SCAN), &pResult, tableGroupId, iaInfo->binfo.pCtx,
                                 numOfOutput, iaInfo->binfo.rowCellInfoOffset, &iaInfo->aggSup, pTaskInfo);
      if (ret != TSDB_CODE_SUCCESS || pResult == NULL) {
        longjmp(pTaskInfo->env, TSDB_CODE_QRY_OUT_OF_MEMORY);
      }
    }
  }
  updateTimeWindowInfo(&iaInfo->twAggSup.timeWindowData, &currWin, true);
  doApplyFunctions(pTaskInfo, iaInfo->binfo.pCtx, &currWin, &iaInfo->twAggSup.timeWindowData, startPos,
                   currPos - startPos, tsCols, pBlock->info.rows, numOfOutput, iaInfo->order);

  outputMergeIntervalResult(pOperatorInfo, tableGroupId, pResultBlock, currTs);
}

static SSDataBlock* doMergeIntervalAgg(SOperatorInfo* pOperator) {
  SExecTaskInfo* pTaskInfo = pOperator->pTaskInfo;

  SMergeIntervalAggOperatorInfo* miaInfo = pOperator->info;
  SIntervalAggOperatorInfo*      iaInfo = &miaInfo->intervalAggOperatorInfo;
  if (pOperator->status == OP_EXEC_DONE) {
    return NULL;
  }

  SSDataBlock* pRes = iaInfo->binfo.pRes;
  blockDataCleanup(pRes);
  blockDataEnsureCapacity(pRes, pOperator->resultInfo.capacity);

  if (!miaInfo->inputBlocksFinished) {
    SOperatorInfo* downstream = pOperator->pDownstream[0];
    int32_t        scanFlag = MAIN_SCAN;
    while (1) {
      SSDataBlock* pBlock = NULL;
      if (miaInfo->prefetchedBlock == NULL) {
        pBlock = downstream->fpSet.getNextFn(downstream);
      } else {
        pBlock = miaInfo->prefetchedBlock;
        miaInfo->groupId = pBlock->info.groupId;
      }

      if (pBlock == NULL) {
        miaInfo->inputBlocksFinished = true;
        break;
      }

      if (!miaInfo->hasGroupId) {
        miaInfo->hasGroupId = true;
        miaInfo->groupId = pBlock->info.groupId;
      } else if (miaInfo->groupId != pBlock->info.groupId) {
        miaInfo->prefetchedBlock = pBlock;
        break;
      }

      getTableScanInfo(pOperator, &iaInfo->order, &scanFlag);
      setInputDataBlock(pOperator, iaInfo->binfo.pCtx, pBlock, iaInfo->order, scanFlag, true);
      doMergeIntervalAggImpl(pOperator, &iaInfo->binfo.resultRowInfo, pBlock, scanFlag, pRes);

      if (pRes->info.rows >= pOperator->resultInfo.threshold) {
        break;
      }
    }

    pRes->info.groupId = miaInfo->groupId;
  }

  if (pRes->info.rows == 0) {
    doSetOperatorCompleted(pOperator);
  }

  size_t rows = pRes->info.rows;
  pOperator->resultInfo.totalRows += rows;
  return (rows == 0) ? NULL : pRes;
}

SOperatorInfo* createMergeIntervalOperatorInfo(SOperatorInfo* downstream, SExprInfo* pExprInfo, int32_t numOfCols,
                                               SSDataBlock* pResBlock, SInterval* pInterval, int32_t primaryTsSlotId,
                                               SExecTaskInfo* pTaskInfo) {
  SMergeIntervalAggOperatorInfo* miaInfo = taosMemoryCalloc(1, sizeof(SMergeIntervalAggOperatorInfo));
  SOperatorInfo*                 pOperator = taosMemoryCalloc(1, sizeof(SOperatorInfo));
  if (miaInfo == NULL || pOperator == NULL) {
    goto _error;
  }

  SIntervalAggOperatorInfo* iaInfo = &miaInfo->intervalAggOperatorInfo;

  iaInfo->win = pTaskInfo->window;
  iaInfo->order = TSDB_ORDER_ASC;
  iaInfo->interval = *pInterval;

  iaInfo->execModel = pTaskInfo->execModel;

  iaInfo->primaryTsIndex = primaryTsSlotId;

  size_t keyBufSize = sizeof(int64_t) + sizeof(int64_t) + POINTER_BYTES;
  initResultSizeInfo(pOperator, 4096);

  int32_t code =
      initAggInfo(&iaInfo->binfo, &iaInfo->aggSup, pExprInfo, numOfCols, pResBlock, keyBufSize, pTaskInfo->id.str);

  initExecTimeWindowInfo(&iaInfo->twAggSup.timeWindowData, &iaInfo->win);

  iaInfo->timeWindowInterpo = timeWindowinterpNeeded(iaInfo->binfo.pCtx, numOfCols, iaInfo);
  if (iaInfo->timeWindowInterpo) {
    iaInfo->binfo.resultRowInfo.openWindow = tdListNew(sizeof(SResultRowPosition));
  }

  //  iaInfo->pTableQueryInfo = initTableQueryInfo(pTableGroupInfo);
  if (code != TSDB_CODE_SUCCESS /* || iaInfo->pTableQueryInfo == NULL*/) {
    goto _error;
  }

  initResultRowInfo(&iaInfo->binfo.resultRowInfo);

  pOperator->name = "TimeMergeIntervalAggOperator";
  pOperator->operatorType = QUERY_NODE_PHYSICAL_PLAN_MERGE_INTERVAL;
  pOperator->blocking = false;
  pOperator->status = OP_NOT_OPENED;
  pOperator->pExpr = pExprInfo;
  pOperator->pTaskInfo = pTaskInfo;
  pOperator->numOfExprs = numOfCols;
  pOperator->info = miaInfo;

  pOperator->fpSet = createOperatorFpSet(operatorDummyOpenFn, doMergeIntervalAgg, NULL, NULL,
                                         destroyMergeIntervalOperatorInfo, NULL, NULL, NULL);

  code = appendDownstream(pOperator, &downstream, 1);
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }

  return pOperator;

_error:
  destroyMergeIntervalOperatorInfo(miaInfo, numOfCols);
  taosMemoryFreeClear(miaInfo);
  taosMemoryFreeClear(pOperator);
  pTaskInfo->code = code;
  return NULL;
}
