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

#define _DEFAULT_SOURCE
#include "tdataformat.h"
#include "tcoding.h"
#include "tdatablock.h"
#include "tlog.h"

static int32_t tGetTagVal(uint8_t *p, STagVal *pTagVal, int8_t isJson);

#pragma pack(push, 1)
typedef struct {
  int16_t nCols;
  uint8_t idx[];
} STSKVRow;
#pragma pack(pop)

#define TSROW_IS_KV_ROW(r) ((r)->flags & TSROW_KV_ROW)
#define BIT1_SIZE(n)       (((n)-1) / 8 + 1)
#define BIT2_SIZE(n)       (((n)-1) / 4 + 1)
#define SET_BIT1(p, i, v)  ((p)[(i) / 8] = (p)[(i) / 8] & (~(((uint8_t)1) << ((i) % 8))) | ((v) << ((i) % 8)))
#define SET_BIT2(p, i, v)  ((p)[(i) / 4] = (p)[(i) / 4] & (~(((uint8_t)3) << ((i) % 4))) | ((v) << ((i) % 4)))
#define GET_BIT1(p, i)     (((p)[(i) / 8] >> ((i) % 8)) & ((uint8_t)1))
#define GET_BIT2(p, i)     (((p)[(i) / 4] >> ((i) % 4)) & ((uint8_t)3))

static FORCE_INLINE int tSKVIdxCmprFn(const void *p1, const void *p2);

// SValue
static FORCE_INLINE int32_t tPutValue(uint8_t *p, SValue *pValue, int8_t type) {
  int32_t n = 0;

  if (IS_VAR_DATA_TYPE(type)) {
    n += tPutBinary(p ? p + n : p, pValue->pData, pValue->nData);
  } else {
    switch (type) {
      case TSDB_DATA_TYPE_BOOL:
        n += tPutI8(p ? p + n : p, pValue->i8 ? 1 : 0);
        break;
      case TSDB_DATA_TYPE_TINYINT:
        n += tPutI8(p ? p + n : p, pValue->i8);
        break;
      case TSDB_DATA_TYPE_SMALLINT:
        n += tPutI16(p ? p + n : p, pValue->i16);
        break;
      case TSDB_DATA_TYPE_INT:
        n += tPutI32(p ? p + n : p, pValue->i32);
        break;
      case TSDB_DATA_TYPE_BIGINT:
        n += tPutI64(p ? p + n : p, pValue->i64);
        break;
      case TSDB_DATA_TYPE_FLOAT:
        n += tPutFloat(p ? p + n : p, pValue->f);
        break;
      case TSDB_DATA_TYPE_DOUBLE:
        n += tPutDouble(p ? p + n : p, pValue->d);
        break;
      case TSDB_DATA_TYPE_TIMESTAMP:
        n += tPutI64(p ? p + n : p, pValue->ts);
        break;
      case TSDB_DATA_TYPE_UTINYINT:
        n += tPutU8(p ? p + n : p, pValue->u8);
        break;
      case TSDB_DATA_TYPE_USMALLINT:
        n += tPutU16(p ? p + n : p, pValue->u16);
        break;
      case TSDB_DATA_TYPE_UINT:
        n += tPutU32(p ? p + n : p, pValue->u32);
        break;
      case TSDB_DATA_TYPE_UBIGINT:
        n += tPutU64(p ? p + n : p, pValue->u64);
        break;
      default:
        ASSERT(0);
    }
  }

  return n;
}

static FORCE_INLINE int32_t tGetValue(uint8_t *p, SValue *pValue, int8_t type) {
  int32_t n = 0;

  if (IS_VAR_DATA_TYPE(type)) {
    n += tGetBinary(p, &pValue->pData, pValue ? &pValue->nData : NULL);
  } else {
    switch (type) {
      case TSDB_DATA_TYPE_BOOL:
        n += tGetI8(p, &pValue->i8);
        break;
      case TSDB_DATA_TYPE_TINYINT:
        n += tGetI8(p, &pValue->i8);
        break;
      case TSDB_DATA_TYPE_SMALLINT:
        n += tGetI16(p, &pValue->i16);
        break;
      case TSDB_DATA_TYPE_INT:
        n += tGetI32(p, &pValue->i32);
        break;
      case TSDB_DATA_TYPE_BIGINT:
        n += tGetI64(p, &pValue->i64);
        break;
      case TSDB_DATA_TYPE_FLOAT:
        n += tGetFloat(p, &pValue->f);
        break;
      case TSDB_DATA_TYPE_DOUBLE:
        n += tGetDouble(p, &pValue->d);
        break;
      case TSDB_DATA_TYPE_TIMESTAMP:
        n += tGetI64(p, &pValue->ts);
        break;
      case TSDB_DATA_TYPE_UTINYINT:
        n += tGetU8(p, &pValue->u8);
        break;
      case TSDB_DATA_TYPE_USMALLINT:
        n += tGetU16(p, &pValue->u16);
        break;
      case TSDB_DATA_TYPE_UINT:
        n += tGetU32(p, &pValue->u32);
        break;
      case TSDB_DATA_TYPE_UBIGINT:
        n += tGetU64(p, &pValue->u64);
        break;
      default:
        ASSERT(0);
    }
  }

  return n;
}

// STSRow2 ========================================================================
static void tTupleTSRowNew(SArray *pArray, STSchema *pTSchema, STSRow2 *pRow) {
  int32_t   nColVal = taosArrayGetSize(pArray);
  STColumn *pTColumn;
  SColVal  *pColVal;

  ASSERT(nColVal > 0);

  pRow->sver = pTSchema->version;

  // ts
  pTColumn = &pTSchema->columns[0];
  pColVal = (SColVal *)taosArrayGet(pArray, 0);

  ASSERT(pTColumn->colId == 0 && pColVal->cid == 0);
  ASSERT(pTColumn->type == TSDB_DATA_TYPE_TIMESTAMP);

  pRow->ts = pColVal->value.ts;

  // other fields
  int32_t  iColVal = 1;
  int32_t  bidx;
  uint32_t nv = 0;
  uint8_t *pb = NULL;
  uint8_t *pf = NULL;
  uint8_t *pv = NULL;
  uint8_t  flags = 0;
  for (int32_t iColumn = 1; iColumn < pTSchema->numOfCols; iColumn++) {
    bidx = iColumn - 1;
    pTColumn = &pTSchema->columns[iColumn];

    if (iColVal < nColVal) {
      pColVal = (SColVal *)taosArrayGet(pArray, iColVal);
    } else {
      pColVal = NULL;
    }

    if (pColVal) {
      if (pColVal->cid == pTColumn->colId) {
        iColVal++;
        if (pColVal->isNone) {
          goto _set_none;
        } else if (pColVal->isNull) {
          goto _set_null;
        } else {
          goto _set_value;
        }
      } else if (pColVal->cid > pTColumn->colId) {
        goto _set_none;
      } else {
        ASSERT(0);
      }
    } else {
      goto _set_none;
    }

  _set_none:
    flags |= TSROW_HAS_NONE;
    // SET_BIT2(pb, bidx, 0); (todo)
    continue;

  _set_null:
    flags != TSROW_HAS_NULL;
    // SET_BIT2(pb, bidx, 1); (todo)
    continue;

  _set_value:
    flags != TSROW_HAS_VAL;
    // SET_BIT2(pb, bidx, 2); (todo)
    if (IS_VAR_DATA_TYPE(pTColumn->type)) {
      // nv += tPutColVal(pv ? pv + nv : pv, pColVal, pTColumn->type, 1);
    } else {
      // tPutColVal(pf ? pf + pTColumn->offset : pf, pColVal, pTColumn->type, 1);
    }
    continue;
  }

  ASSERT(flags);
  switch (flags & 0xf) {
    case TSROW_HAS_NONE:
    case TSROW_HAS_NULL:
      pRow->nData = 0;
      break;
    case TSROW_HAS_VAL:
      pRow->nData = pTSchema->flen + nv;
      break;
    case TSROW_HAS_NULL | TSROW_HAS_NONE:
      pRow->nData = BIT1_SIZE(pTSchema->numOfCols - 1);
      break;
    case TSROW_HAS_VAL | TSROW_HAS_NONE:
    case TSROW_HAS_VAL | TSROW_HAS_NULL:
      pRow->nData = BIT1_SIZE(pTSchema->numOfCols - 1) + pTSchema->flen + nv;
      break;
    case TSROW_HAS_VAL | TSROW_HAS_NULL | TSROW_HAS_NONE:
      pRow->nData = BIT2_SIZE(pTSchema->numOfCols - 1) + pTSchema->flen + nv;
      break;
    default:
      break;
  }
}

static void tMapTSRowNew(SArray *pArray, STSchema *pTSchema, STSRow2 *pRow) {
  int32_t   nColVal = taosArrayGetSize(pArray);
  STColumn *pTColumn;
  SColVal  *pColVal;

  ASSERT(nColVal > 0);

  pRow->sver = pTSchema->version;

  // ts
  pTColumn = &pTSchema->columns[0];
  pColVal = (SColVal *)taosArrayGet(pArray, 0);

  ASSERT(pTColumn->colId == 0 && pColVal->cid == 0);
  ASSERT(pTColumn->type == TSDB_DATA_TYPE_TIMESTAMP);

  pRow->ts = pColVal->value.ts;

  // other fields
  int32_t  iColVal = 1;
  uint32_t nv = 0;
  uint8_t *pv = NULL;
  uint8_t *pidx = NULL;
  uint8_t  flags = 0;
  int16_t  nCol = 0;
  for (int32_t iColumn = 1; iColumn < pTSchema->numOfCols; iColumn++) {
    pTColumn = &pTSchema->columns[iColumn];

    if (iColVal < nColVal) {
      pColVal = (SColVal *)taosArrayGet(pArray, iColVal);
    } else {
      pColVal = NULL;
    }

    if (pColVal) {
      if (pColVal->cid == pTColumn->colId) {
        iColVal++;
        if (pColVal->isNone) {
          goto _set_none;
        } else if (pColVal->isNull) {
          goto _set_null;
        } else {
          goto _set_value;
        }
      } else if (pColVal->cid > pTColumn->colId) {
        goto _set_none;
      } else {
        ASSERT(0);
      }
    } else {
      goto _set_none;
    }

  _set_none:
    flags |= TSROW_HAS_NONE;
    continue;

  _set_null:
    flags != TSROW_HAS_NULL;
    pidx[nCol++] = nv;
    // nv += tPutColVal(pv ? pv + nv : pv, pColVal, pTColumn->type, 0);
    continue;

  _set_value:
    flags != TSROW_HAS_VAL;
    pidx[nCol++] = nv;
    // nv += tPutColVal(pv ? pv + nv : pv, pColVal, pTColumn->type, 0);
    continue;
  }

  if (nv <= UINT8_MAX) {
    // small
  } else if (nv <= UINT16_MAX) {
    // mid
  } else {
    // large
  }
}

// try-decide-build
int32_t tTSRowNew(SArray *pArray, STSchema *pTSchema, STSRow2 **ppRow) {
  int32_t code = 0;
  STSRow2 rowT = {0};
  STSRow2 rowM = {0};

  // try
  tTupleTSRowNew(pArray, pTSchema, &rowT);
  tMapTSRowNew(pArray, pTSchema, &rowM);

  // decide & build
  if (rowT.nData <= rowM.nData) {
    tTupleTSRowNew(pArray, pTSchema, &rowT);
  } else {
    tMapTSRowNew(pArray, pTSchema, &rowM);
  }

  return code;
}

int32_t tTSRowClone(const STSRow2 *pRow, STSRow2 **ppRow) {
  int32_t code = 0;

  (*ppRow) = (STSRow2 *)taosMemoryMalloc(sizeof(**ppRow));
  if (*ppRow == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }
  **ppRow = *pRow;
  (*ppRow)->pData = NULL;

  if (pRow->nData) {
    (*ppRow)->pData = taosMemoryMalloc(pRow->nData);
    if ((*ppRow)->pData == NULL) {
      taosMemoryFree(*ppRow);
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _exit;
    }
    memcpy((*ppRow)->pData, pRow->pData, pRow->nData);
  }

_exit:
  return code;
}

void tTSRowFree(STSRow2 *pRow) {
  if (pRow) {
    if (pRow->pData) taosMemoryFree(pRow->pData);
    taosMemoryFree(pRow);
  }
}

void tTSRowGet(STSRow2 *pRow, STSchema *pTSchema, int32_t iCol, SColVal *pColVal) {
  uint8_t   isTuple = (pRow->flags & 0xf0 == 0) ? 1 : 0;
  STColumn *pTColumn = &pTSchema->columns[iCol];
  uint8_t   flags = pRow->flags & (uint8_t)0xf;
  SValue    value;

  ASSERT(iCol < pTSchema->numOfCols);
  ASSERT(flags);
  ASSERT(pRow->sver == pTSchema->version);

  if (iCol == 0) {
    value.ts = pRow->ts;
    goto _return_value;
  }

  if (flags == TSROW_HAS_NONE) {
    goto _return_none;
  } else if (flags == TSROW_HAS_NONE) {
    goto _return_null;
  }

  ASSERT(pRow->nData && pRow->pData);

  if (isTuple) {
    uint8_t *pb = pRow->pData;
    uint8_t *pf = NULL;
    uint8_t *pv = NULL;
    uint8_t *p;
    uint8_t  b;

    // bit
    switch (flags) {
      case TSROW_HAS_VAL:
        pf = pb;
        break;
      case TSROW_HAS_NULL | TSROW_HAS_NONE:
        b = GET_BIT1(pb, iCol - 1);
        if (b == 0) {
          goto _return_none;
        } else {
          goto _return_null;
        }
      case TSROW_HAS_VAL | TSROW_HAS_NONE:
        b = GET_BIT1(pb, iCol - 1);
        if (b == 0) {
          goto _return_none;
        } else {
          pf = pb + BIT1_SIZE(pTSchema->numOfCols - 1);
          break;
        }
      case TSROW_HAS_VAL | TSROW_HAS_NULL:
        b = GET_BIT1(pb, iCol - 1);
        if (b == 0) {
          goto _return_null;
        } else {
          pf = pb + BIT1_SIZE(pTSchema->numOfCols - 1);
          break;
        }
      case TSROW_HAS_VAL | TSROW_HAS_NULL | TSROW_HAS_NONE:
        b = GET_BIT2(pb, iCol - 1);
        if (b == 0) {
          goto _return_none;
        } else if (b == 1) {
          goto _return_null;
        } else {
          pf = pb + BIT2_SIZE(pTSchema->numOfCols - 1);
          break;
        }
      default:
        ASSERT(0);
    }

    ASSERT(pf);

    p = pf + pTColumn->offset;
    if (IS_VAR_DATA_TYPE(pTColumn->type)) {
      pv = pf + pTSchema->flen;
      p = pv + *(VarDataOffsetT *)p;
    }
    tGetValue(p, &value, pTColumn->type);
    goto _return_value;
  } else {
    STSKVRow *pRowK = (STSKVRow *)pRow->pData;
    int16_t   lidx = 0;
    int16_t   ridx = pRowK->nCols - 1;
    uint8_t  *p;
    int16_t   midx;
    uint32_t  n;
    int16_t   cid;

    ASSERT(pRowK->nCols > 0);

    if (pRow->flags & TSROW_KV_SMALL) {
      p = pRow->pData + sizeof(STSKVRow) + sizeof(uint8_t) * pRowK->nCols;
    } else if (pRow->flags & TSROW_KV_MID) {
      p = pRow->pData + sizeof(STSKVRow) + sizeof(uint16_t) * pRowK->nCols;
    } else if (pRow->flags & TSROW_KV_BIG) {
      p = pRow->pData + sizeof(STSKVRow) + sizeof(uint32_t) * pRowK->nCols;
    } else {
      ASSERT(0);
    }
    while (lidx <= ridx) {
      midx = (lidx + ridx) / 2;

      if (pRow->flags & TSROW_KV_SMALL) {
        n = ((uint8_t *)pRowK->idx)[midx];
      } else if (pRow->flags & TSROW_KV_MID) {
        n = ((uint16_t *)pRowK->idx)[midx];
      } else {
        n = ((uint32_t *)pRowK->idx)[midx];
      }

      n += tGetI16v(p + n, &cid);

      if (TABS(cid) == pTColumn->colId) {
        if (cid < 0) {
          goto _return_null;
        } else {
          n += tGetValue(p + n, &value, pTColumn->type);
          goto _return_value;
        }

        return;
      } else if (TABS(cid) > pTColumn->colId) {
        ridx = midx - 1;
      } else {
        lidx = midx + 1;
      }
    }

    // not found, return NONE
    goto _return_none;
  }

_return_none:
  *pColVal = COL_VAL_NONE(pTColumn->colId);
  return;

_return_null:
  *pColVal = COL_VAL_NULL(pTColumn->colId);
  return;

_return_value:
  *pColVal = COL_VAL_VALUE(pTColumn->colId, value);
  return;
}

int32_t tTSRowToArray(STSRow2 *pRow, STSchema *pTSchema, SArray **ppArray) {
  int32_t code = 0;
  SColVal cv;

  (*ppArray) = taosArrayInit(pTSchema->numOfCols, sizeof(SColVal));
  if (*ppArray == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }

  for (int32_t iColumn = 0; iColumn < pTSchema->numOfCols; iColumn++) {
    tTSRowGet(pRow, pTSchema, iColumn, &cv);
    taosArrayPush(*ppArray, &cv);
  }

_exit:
  return code;
}

int32_t tPutTSRow(uint8_t *p, STSRow2 *pRow) {
  int32_t n = 0;

  n += tPutI64(p ? p + n : p, pRow->ts);
  n += tPutI8(p ? p + n : p, pRow->flags);
  n += tPutI32v(p ? p + n : p, pRow->sver);

  ASSERT(pRow->flags & 0xf);

  switch (pRow->flags & 0xf) {
    case TSROW_HAS_NONE:
    case TSROW_HAS_NULL:
      ASSERT(pRow->nData == 0);
      ASSERT(pRow->pData == NULL);
      break;
    default:
      ASSERT(pRow->nData && pRow->pData);
      n += tPutBinary(p ? p + n : p, pRow->pData, pRow->nData);
      break;
  }

  return n;
}

int32_t tGetTSRow(uint8_t *p, STSRow2 *pRow) {
  int32_t n = 0;

  n += tGetI64(p + n, &pRow->ts);
  n += tGetI8(p + n, &pRow->flags);
  n += tGetI32v(p + n, &pRow->sver);

  ASSERT(pRow->flags);
  switch (pRow->flags & 0xf) {
    case TSROW_HAS_NONE:
    case TSROW_HAS_NULL:
      pRow->nData = 0;
      pRow->pData = NULL;
      break;
    default:
      n += tGetBinary(p + n, &pRow->pData, &pRow->nData);
      break;
  }

  return n;
}

// STSchema
int32_t tTSchemaCreate(int32_t sver, SSchema *pSchema, int32_t ncols, STSchema **ppTSchema) {
  *ppTSchema = (STSchema *)taosMemoryMalloc(sizeof(STSchema) + sizeof(STColumn) * ncols);
  if (*ppTSchema == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }

  (*ppTSchema)->numOfCols = ncols;
  (*ppTSchema)->version = sver;
  (*ppTSchema)->flen = 0;
  (*ppTSchema)->vlen = 0;
  (*ppTSchema)->tlen = 0;

  for (int32_t iCol = 0; iCol < ncols; iCol++) {
    SSchema  *pColumn = &pSchema[iCol];
    STColumn *pTColumn = &((*ppTSchema)->columns[iCol]);

    pTColumn->colId = pColumn->colId;
    pTColumn->type = pColumn->type;
    pTColumn->flags = pColumn->flags;
    pTColumn->bytes = pColumn->bytes;
    pTColumn->offset = (*ppTSchema)->flen;

    // skip first column
    if (iCol) {
      (*ppTSchema)->flen += TYPE_BYTES[pColumn->type];
      if (IS_VAR_DATA_TYPE(pColumn->type)) {
        (*ppTSchema)->vlen += (pColumn->bytes + 5);
      }
    }
  }

  return 0;
}

void tTSchemaDestroy(STSchema *pTSchema) {
  if (pTSchema) taosMemoryFree(pTSchema);
}

// STSRowBuilder
#if 0
int32_t tTSRowBuilderInit(STSRowBuilder *pBuilder, int32_t sver, int32_t nCols, SSchema *pSchema) {
  if (tTSchemaCreate(sver, pSchema, nCols, &pBuilder->pTSchema) < 0) return -1;

  pBuilder->szBitMap1 = BIT1_SIZE(nCols - 1);
  pBuilder->szBitMap2 = BIT2_SIZE(nCols - 1);
  pBuilder->szKVBuf =
      sizeof(STSKVRow) + sizeof(SKVIdx) * (nCols - 1) + pBuilder->pTSchema->flen + pBuilder->pTSchema->vlen;
  pBuilder->szTPBuf = pBuilder->szBitMap2 + pBuilder->pTSchema->flen + pBuilder->pTSchema->vlen;
  pBuilder->pKVBuf = taosMemoryMalloc(pBuilder->szKVBuf);
  if (pBuilder->pKVBuf == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    tTSchemaDestroy(pBuilder->pTSchema);
    return -1;
  }
  pBuilder->pTPBuf = taosMemoryMalloc(pBuilder->szTPBuf);
  if (pBuilder->pTPBuf == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    taosMemoryFree(pBuilder->pKVBuf);
    tTSchemaDestroy(pBuilder->pTSchema);
    return -1;
  }

  return 0;
}

void tTSRowBuilderClear(STSRowBuilder *pBuilder) {
  if (pBuilder->pTPBuf) {
    taosMemoryFree(pBuilder->pTPBuf);
    pBuilder->pTPBuf = NULL;
  }
  if (pBuilder->pKVBuf) {
    taosMemoryFree(pBuilder->pKVBuf);
    pBuilder->pKVBuf = NULL;
  }
  tTSchemaDestroy(pBuilder->pTSchema);
  pBuilder->pTSchema = NULL;
}

void tTSRowBuilderReset(STSRowBuilder *pBuilder) {
  for (int32_t iCol = pBuilder->pTSchema->numOfCols - 1; iCol >= 0; iCol--) {
    STColumn *pTColumn = &pBuilder->pTSchema->columns[iCol];
    COL_CLR_SET(pTColumn->flags);
  }

  pBuilder->iCol = 0;
  ((STSKVRow *)pBuilder->pKVBuf)->nCols = 0;
  pBuilder->vlenKV = 0;
  pBuilder->vlenTP = 0;
  pBuilder->row.flags = 0;
}

int32_t tTSRowBuilderPut(STSRowBuilder *pBuilder, int32_t cid, uint8_t *pData, uint32_t nData) {
  STColumn *pTColumn = &pBuilder->pTSchema->columns[pBuilder->iCol];
  uint8_t  *p;
  int32_t   iCol;
  STSKVRow *pTSKVRow = (STSKVRow *)pBuilder->pKVBuf;

  // use interp search
  if (pTColumn->colId < cid) {  // right search
    for (iCol = pBuilder->iCol + 1; iCol < pBuilder->pTSchema->numOfCols; iCol++) {
      pTColumn = &pBuilder->pTSchema->columns[iCol];
      if (pTColumn->colId >= cid) break;
    }
  } else if (pTColumn->colId > cid) {  // left search
    for (iCol = pBuilder->iCol - 1; iCol >= 0; iCol--) {
      pTColumn = &pBuilder->pTSchema->columns[iCol];
      if (pTColumn->colId <= cid) break;
    }
  }

  if (pTColumn->colId != cid || COL_IS_SET(pTColumn->flags)) {
    return -1;
  }

  pBuilder->iCol = iCol;

  // set value
  if (cid == 0) {
    ASSERT(pData && nData == sizeof(TSKEY) && iCol == 0);
    pBuilder->row.ts = *(TSKEY *)pData;
    pTColumn->flags |= COL_SET_VAL;
  } else {
    if (pData) {
      // set VAL

      pBuilder->row.flags |= TSROW_HAS_VAL;
      pTColumn->flags |= COL_SET_VAL;

      /* KV */
      if (1) {  // avoid KV at some threshold (todo)
        pTSKVRow->idx[pTSKVRow->nCols].cid = cid;
        pTSKVRow->idx[pTSKVRow->nCols].offset = pBuilder->vlenKV;

        p = pBuilder->pKVBuf + sizeof(STSKVRow) + sizeof(SKVIdx) * (pBuilder->pTSchema->numOfCols - 1) +
            pBuilder->vlenKV;
        if (IS_VAR_DATA_TYPE(pTColumn->type)) {
          ASSERT(nData <= pTColumn->bytes);
          pBuilder->vlenKV += tPutBinary(p, pData, nData);
        } else {
          ASSERT(nData == pTColumn->bytes);
          memcpy(p, pData, nData);
          pBuilder->vlenKV += nData;
        }
      }

      /* TUPLE */
      p = pBuilder->pTPBuf + pBuilder->szBitMap2 + pTColumn->offset;
      if (IS_VAR_DATA_TYPE(pTColumn->type)) {
        ASSERT(nData <= pTColumn->bytes);
        *(int32_t *)p = pBuilder->vlenTP;

        p = pBuilder->pTPBuf + pBuilder->szBitMap2 + pBuilder->pTSchema->flen + pBuilder->vlenTP;
        pBuilder->vlenTP += tPutBinary(p, pData, nData);
      } else {
        ASSERT(nData == pTColumn->bytes);
        memcpy(p, pData, nData);
      }
    } else {
      // set NULL

      pBuilder->row.flags |= TSROW_HAS_NULL;
      pTColumn->flags |= COL_SET_NULL;

      pTSKVRow->idx[pTSKVRow->nCols].cid = cid;
      pTSKVRow->idx[pTSKVRow->nCols].offset = -1;
    }

    pTSKVRow->nCols++;
  }

  return 0;
}

static FORCE_INLINE int tSKVIdxCmprFn(const void *p1, const void *p2) {
  SKVIdx *pKVIdx1 = (SKVIdx *)p1;
  SKVIdx *pKVIdx2 = (SKVIdx *)p2;
  if (pKVIdx1->cid > pKVIdx2->cid) {
    return 1;
  } else if (pKVIdx1->cid < pKVIdx2->cid) {
    return -1;
  }
  return 0;
}
static void setBitMap(uint8_t *p, STSchema *pTSchema, uint8_t flags) {
  int32_t   bidx;
  STColumn *pTColumn;

  for (int32_t iCol = 1; iCol < pTSchema->numOfCols; iCol++) {
    pTColumn = &pTSchema->columns[iCol];
    bidx = iCol - 1;

    switch (flags) {
      case TSROW_HAS_NULL | TSROW_HAS_NONE:
        if (pTColumn->flags & COL_SET_NULL) {
          SET_BIT1(p, bidx, (uint8_t)1);
        } else {
          SET_BIT1(p, bidx, (uint8_t)0);
        }
        break;
      case TSROW_HAS_VAL | TSROW_HAS_NULL | TSROW_HAS_NONE:
        if (pTColumn->flags & COL_SET_NULL) {
          SET_BIT2(p, bidx, (uint8_t)1);
        } else if (pTColumn->flags & COL_SET_VAL) {
          SET_BIT2(p, bidx, (uint8_t)2);
        } else {
          SET_BIT2(p, bidx, (uint8_t)0);
        }
        break;
      default:
        if (pTColumn->flags & COL_SET_VAL) {
          SET_BIT1(p, bidx, (uint8_t)1);
        } else {
          SET_BIT1(p, bidx, (uint8_t)0);
        }

        break;
    }
  }
}
int32_t tTSRowBuilderGetRow(STSRowBuilder *pBuilder, const STSRow2 **ppRow) {
  int32_t   nDataTP, nDataKV;
  STSKVRow *pTSKVRow = (STSKVRow *)pBuilder->pKVBuf;
  int32_t   nCols = pBuilder->pTSchema->numOfCols;

  // error not set ts
  if (!COL_IS_SET(pBuilder->pTSchema->columns->flags)) {
    return -1;
  }

  ASSERT(pTSKVRow->nCols < nCols);
  if (pTSKVRow->nCols < nCols - 1) {
    pBuilder->row.flags |= TSROW_HAS_NONE;
  }

  ASSERT((pBuilder->row.flags & 0xf) != 0);
  *(ppRow) = &pBuilder->row;
  switch (pBuilder->row.flags & 0xf) {
    case TSROW_HAS_NONE:
    case TSROW_HAS_NULL:
      pBuilder->row.nData = 0;
      pBuilder->row.pData = NULL;
      return 0;
    case TSROW_HAS_NULL | TSROW_HAS_NONE:
      nDataTP = pBuilder->szBitMap1;
      break;
    case TSROW_HAS_VAL:
      nDataTP = pBuilder->pTSchema->flen + pBuilder->vlenTP;
      break;
    case TSROW_HAS_VAL | TSROW_HAS_NONE:
    case TSROW_HAS_VAL | TSROW_HAS_NULL:
      nDataTP = pBuilder->szBitMap1 + pBuilder->pTSchema->flen + pBuilder->vlenTP;
      break;
    case TSROW_HAS_VAL | TSROW_HAS_NULL | TSROW_HAS_NONE:
      nDataTP = pBuilder->szBitMap2 + pBuilder->pTSchema->flen + pBuilder->vlenTP;
      break;
    default:
      ASSERT(0);
  }

  nDataKV = sizeof(STSKVRow) + sizeof(SKVIdx) * pTSKVRow->nCols + pBuilder->vlenKV;
  pBuilder->row.sver = pBuilder->pTSchema->version;
  if (nDataKV < nDataTP) {
    // generate KV row

    ASSERT((pBuilder->row.flags & 0xf) != TSROW_HAS_VAL);

    pBuilder->row.flags |= TSROW_KV_ROW;
    pBuilder->row.nData = nDataKV;
    pBuilder->row.pData = pBuilder->pKVBuf;

    qsort(pTSKVRow->idx, pTSKVRow->nCols, sizeof(SKVIdx), tSKVIdxCmprFn);
    if (pTSKVRow->nCols < nCols - 1) {
      memmove(&pTSKVRow->idx[pTSKVRow->nCols], &pTSKVRow->idx[nCols - 1], pBuilder->vlenKV);
    }
  } else {
    // generate TUPLE row

    pBuilder->row.nData = nDataTP;

    uint8_t *p;
    uint8_t  flags = (pBuilder->row.flags & 0xf);

    if (flags == TSROW_HAS_VAL) {
      pBuilder->row.pData = pBuilder->pTPBuf + pBuilder->szBitMap2;
    } else {
      if (flags == (TSROW_HAS_VAL | TSROW_HAS_NULL | TSROW_HAS_NONE)) {
        pBuilder->row.pData = pBuilder->pTPBuf;
      } else {
        pBuilder->row.pData = pBuilder->pTPBuf + pBuilder->szBitMap2 - pBuilder->szBitMap1;
      }

      setBitMap(pBuilder->row.pData, pBuilder->pTSchema, flags);
    }
  }

  return 0;
}
#endif

static int tTagValCmprFn(const void *p1, const void *p2) {
  if (((STagVal *)p1)->cid < ((STagVal *)p2)->cid) {
    return -1;
  } else if (((STagVal *)p1)->cid > ((STagVal *)p2)->cid) {
    return 1;
  }

  return 0;
}
static int tTagValJsonCmprFn(const void *p1, const void *p2) {
  return strcmp(((STagVal *)p1)[0].pKey, ((STagVal *)p2)[0].pKey);
}

static void debugPrintTagVal(int8_t type, const void *val, int32_t vlen, const char *tag, int32_t ln) {
  switch (type) {
    case TSDB_DATA_TYPE_JSON:
    case TSDB_DATA_TYPE_VARCHAR:
    case TSDB_DATA_TYPE_NCHAR: {
      char tmpVal[32] = {0};
      strncpy(tmpVal, val, vlen > 31 ? 31 : vlen);
      printf("%s:%d type:%d vlen:%d, val:\"%s\"\n", tag, ln, (int32_t)type, vlen, tmpVal);
    } break;
    case TSDB_DATA_TYPE_FLOAT:
      printf("%s:%d type:%d vlen:%d, val:%f\n", tag, ln, (int32_t)type, vlen, *(float *)val);
      break;
    case TSDB_DATA_TYPE_DOUBLE:
      printf("%s:%d type:%d vlen:%d, val:%lf\n", tag, ln, (int32_t)type, vlen, *(double *)val);
      break;
    case TSDB_DATA_TYPE_BOOL:
      printf("%s:%d type:%d vlen:%d, val:%" PRIu8 "\n", tag, ln, (int32_t)type, vlen, *(uint8_t *)val);
      break;
    case TSDB_DATA_TYPE_TINYINT:
      printf("%s:%d type:%d vlen:%d, val:%" PRIi8 "\n", tag, ln, (int32_t)type, vlen, *(int8_t *)val);
      break;
    case TSDB_DATA_TYPE_SMALLINT:
      printf("%s:%d type:%d vlen:%d, val:%" PRIi16 "\n", tag, ln, (int32_t)type, vlen, *(int16_t *)val);
      break;
    case TSDB_DATA_TYPE_INT:
      printf("%s:%d type:%d vlen:%d, val:%" PRIi32 "\n", tag, ln, (int32_t)type, vlen, *(int32_t *)val);
      break;
    case TSDB_DATA_TYPE_BIGINT:
      printf("%s:%d type:%d vlen:%d, val:%" PRIi64 "\n", tag, ln, (int32_t)type, vlen, *(int64_t *)val);
      break;
    case TSDB_DATA_TYPE_TIMESTAMP:
      printf("%s:%d type:%d vlen:%d, val:%" PRIi64 "\n", tag, ln, (int32_t)type, vlen, *(int64_t *)val);
      break;
    case TSDB_DATA_TYPE_UTINYINT:
      printf("%s:%d type:%d vlen:%d, val:%" PRIu8 "\n", tag, ln, (int32_t)type, vlen, *(uint8_t *)val);
      break;
    case TSDB_DATA_TYPE_USMALLINT:
      printf("%s:%d type:%d vlen:%d, val:%" PRIu16 "\n", tag, ln, (int32_t)type, vlen, *(uint16_t *)val);
      break;
    case TSDB_DATA_TYPE_UINT:
      printf("%s:%d type:%d vlen:%d, val:%" PRIu32 "\n", tag, ln, (int32_t)type, vlen, *(uint32_t *)val);
      break;
    case TSDB_DATA_TYPE_UBIGINT:
      printf("%s:%d type:%d vlen:%d, val:%" PRIu64 "\n", tag, ln, (int32_t)type, vlen, *(uint64_t *)val);
      break;
    case TSDB_DATA_TYPE_NULL:
      printf("%s:%d type:%d vlen:%d, val:%" PRIi8 "\n", tag, ln, (int32_t)type, vlen, *(int8_t *)val);
      break;
    default:
      ASSERT(0);
      break;
  }
}

// if (isLarge) {
//   p = (uint8_t *)&((int16_t *)pTag->idx)[pTag->nTag];
// } else {
//   p = (uint8_t *)&pTag->idx[pTag->nTag];
// }

// (*ppArray) = taosArrayInit(pTag->nTag + 1, sizeof(STagVal));
// if (*ppArray == NULL) {
//   code = TSDB_CODE_OUT_OF_MEMORY;
//   goto _err;
// }

// for (int16_t iTag = 0; iTag < pTag->nTag; iTag++) {
//   if (isLarge) {
//     offset = ((int16_t *)pTag->idx)[iTag];
//   } else {
//     offset = pTag->idx[iTag];
//   }

void debugPrintSTag(STag *pTag, const char *tag, int32_t ln) {
  int8_t   isJson = pTag->flags & TD_TAG_JSON;
  int8_t   isLarge = pTag->flags & TD_TAG_LARGE;
  uint8_t *p = NULL;
  int16_t  offset = 0;

  if (isLarge) {
    p = (uint8_t *)&((int16_t *)pTag->idx)[pTag->nTag];
  } else {
    p = (uint8_t *)&pTag->idx[pTag->nTag];
  }
  printf("%s:%d >>> STAG === %s:%s, len: %d, nTag: %d, sver:%d\n", tag, ln, isJson ? "json" : "normal",
         isLarge ? "large" : "small", (int32_t)pTag->len, (int32_t)pTag->nTag, pTag->ver);
  for (uint16_t n = 0; n < pTag->nTag; ++n) {
    if (isLarge) {
      offset = ((int16_t *)pTag->idx)[n];
    } else {
      offset = pTag->idx[n];
    }
    STagVal tagVal = {0};
    if (isJson) {
      tagVal.pKey = (char *)POINTER_SHIFT(p, offset);
    } else {
      tagVal.cid = *(int16_t *)POINTER_SHIFT(p, offset);
    }
    printf("%s:%d loop[%d-%d] offset=%d\n", __func__, __LINE__, (int32_t)pTag->nTag, (int32_t)n, (int32_t)offset);
    tGetTagVal(p + offset, &tagVal, isJson);
    if (IS_VAR_DATA_TYPE(tagVal.type)) {
      debugPrintTagVal(tagVal.type, tagVal.pData, tagVal.nData, __func__, __LINE__);
    } else {
      debugPrintTagVal(tagVal.type, &tagVal.i64, tDataTypes[tagVal.type].bytes, __func__, __LINE__);
    }
  }
  printf("\n");
}

static int32_t tPutTagVal(uint8_t *p, STagVal *pTagVal, int8_t isJson) {
  int32_t n = 0;

  // key
  if (isJson) {
    n += tPutCStr(p ? p + n : p, pTagVal->pKey);
  } else {
    n += tPutI16v(p ? p + n : p, pTagVal->cid);
  }

  // type
  n += tPutI8(p ? p + n : p, pTagVal->type);

  // value
  if (IS_VAR_DATA_TYPE(pTagVal->type)) {
    n += tPutBinary(p ? p + n : p, pTagVal->pData, pTagVal->nData);
  } else {
    p = p ? p + n : p;
    n += tDataTypes[pTagVal->type].bytes;
    if (p) memcpy(p, &(pTagVal->i64), tDataTypes[pTagVal->type].bytes);
  }

  return n;
}
static int32_t tGetTagVal(uint8_t *p, STagVal *pTagVal, int8_t isJson) {
  int32_t n = 0;

  // key
  if (isJson) {
    n += tGetCStr(p + n, &pTagVal->pKey);
  } else {
    n += tGetI16v(p + n, &pTagVal->cid);
  }

  // type
  n += tGetI8(p + n, &pTagVal->type);

  // value
  if (IS_VAR_DATA_TYPE(pTagVal->type)) {
    n += tGetBinary(p + n, &pTagVal->pData, &pTagVal->nData);
  } else {
    memcpy(&(pTagVal->i64), p + n, tDataTypes[pTagVal->type].bytes);
    n += tDataTypes[pTagVal->type].bytes;
  }

  return n;
}
int32_t tTagNew(SArray *pArray, int32_t version, int8_t isJson, STag **ppTag) {
  int32_t  code = 0;
  uint8_t *p = NULL;
  int16_t  n = 0;
  int16_t  nTag = taosArrayGetSize(pArray);
  int32_t  szTag = 0;
  int8_t   isLarge = 0;

  // sort
  if (isJson) {
    qsort(pArray->pData, nTag, sizeof(STagVal), tTagValJsonCmprFn);
  } else {
    qsort(pArray->pData, nTag, sizeof(STagVal), tTagValCmprFn);
  }

  // get size
  for (int16_t iTag = 0; iTag < nTag; iTag++) {
    szTag += tPutTagVal(NULL, (STagVal *)taosArrayGet(pArray, iTag), isJson);
  }
  if (szTag <= INT8_MAX) {
    szTag = szTag + sizeof(STag) + sizeof(int8_t) * nTag;
  } else {
    szTag = szTag + sizeof(STag) + sizeof(int16_t) * nTag;
    isLarge = 1;
  }

  ASSERT(szTag <= INT16_MAX);

  // build tag
  (*ppTag) = (STag *)taosMemoryCalloc(szTag, 1);
  if ((*ppTag) == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  (*ppTag)->flags = 0;
  if (isJson) {
    (*ppTag)->flags |= TD_TAG_JSON;
  }
  if (isLarge) {
    (*ppTag)->flags |= TD_TAG_LARGE;
  }
  (*ppTag)->len = szTag;
  (*ppTag)->nTag = nTag;
  (*ppTag)->ver = version;

  if (isLarge) {
    p = (uint8_t *)&((int16_t *)(*ppTag)->idx)[nTag];
  } else {
    p = (uint8_t *)&(*ppTag)->idx[nTag];
  }
  n = 0;
  for (int16_t iTag = 0; iTag < nTag; iTag++) {
    if (isLarge) {
      ((int16_t *)(*ppTag)->idx)[iTag] = n;
    } else {
      (*ppTag)->idx[iTag] = n;
    }
    n += tPutTagVal(p + n, (STagVal *)taosArrayGet(pArray, iTag), isJson);
  }

  debugPrintSTag(*ppTag, __func__, __LINE__);

  return code;

_err:
  return code;
}

void tTagFree(STag *pTag) {
  if (pTag) taosMemoryFree(pTag);
}

char *tTagValToData(const STagVal *value, bool isJson) {
  if (!value) return NULL;
  char  *data = NULL;
  int8_t typeBytes = 0;
  if (isJson) {
    typeBytes = CHAR_BYTES;
  }
  if (IS_VAR_DATA_TYPE(value->type)) {
    data = taosMemoryCalloc(1, typeBytes + VARSTR_HEADER_SIZE + value->nData);
    if (data == NULL) return NULL;
    if (isJson) *data = value->type;
    varDataLen(data + typeBytes) = value->nData;
    memcpy(varDataVal(data + typeBytes), value->pData, value->nData);
  } else {
    data = ((char *)&(value->i64)) - typeBytes;  // json with type
  }

  return data;
}

bool tTagGet(const STag *pTag, STagVal *pTagVal) {
  int16_t  lidx = 0;
  int16_t  ridx = pTag->nTag - 1;
  int16_t  midx;
  uint8_t *p;
  int8_t   isJson = pTag->flags & TD_TAG_JSON;
  int8_t   isLarge = pTag->flags & TD_TAG_LARGE;
  int16_t  offset;
  STagVal  tv;
  int      c;

  if (isLarge) {
    p = (uint8_t *)&((int16_t *)pTag->idx)[pTag->nTag];
  } else {
    p = (uint8_t *)&pTag->idx[pTag->nTag];
  }

  pTagVal->type = TSDB_DATA_TYPE_NULL;
  pTagVal->pData = NULL;
  pTagVal->nData = 0;
  while (lidx <= ridx) {
    midx = (lidx + ridx) / 2;
    if (isLarge) {
      offset = ((int16_t *)pTag->idx)[midx];
    } else {
      offset = pTag->idx[midx];
    }

    tGetTagVal(p + offset, &tv, isJson);
    if (isJson) {
      c = tTagValJsonCmprFn(pTagVal, &tv);
    } else {
      c = tTagValCmprFn(pTagVal, &tv);
    }

    if (c < 0) {
      ridx = midx - 1;
    } else if (c > 0) {
      lidx = midx + 1;
    } else {
      memcpy(pTagVal, &tv, sizeof(tv));
      return true;
    }
  }
  return false;
}

int32_t tEncodeTag(SEncoder *pEncoder, const STag *pTag) {
  return tEncodeBinary(pEncoder, (const uint8_t *)pTag, pTag->len);
}

int32_t tDecodeTag(SDecoder *pDecoder, STag **ppTag) {
  uint32_t len = 0;
  return tDecodeBinary(pDecoder, (uint8_t **)ppTag, &len);
}

int32_t tTagToValArray(const STag *pTag, SArray **ppArray) {
  int32_t  code = 0;
  uint8_t *p = NULL;
  STagVal  tv = {0};
  int8_t   isLarge = pTag->flags & TD_TAG_LARGE;
  int16_t  offset = 0;

  if (isLarge) {
    p = (uint8_t *)&((int16_t *)pTag->idx)[pTag->nTag];
  } else {
    p = (uint8_t *)&pTag->idx[pTag->nTag];
  }

  (*ppArray) = taosArrayInit(pTag->nTag + 1, sizeof(STagVal));
  if (*ppArray == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }

  for (int16_t iTag = 0; iTag < pTag->nTag; iTag++) {
    if (isLarge) {
      offset = ((int16_t *)pTag->idx)[iTag];
    } else {
      offset = pTag->idx[iTag];
    }
    tGetTagVal(p + offset, &tv, pTag->flags & TD_TAG_JSON);
    taosArrayPush(*ppArray, &tv);
  }

  return code;

_err:
  return code;
}

#if 1  // ===================================================================================================================
static void dataColSetNEleNull(SDataCol *pCol, int nEle);
int         tdAllocMemForCol(SDataCol *pCol, int maxPoints) {
  int spaceNeeded = pCol->bytes * maxPoints;
  if (IS_VAR_DATA_TYPE(pCol->type)) {
    spaceNeeded += sizeof(VarDataOffsetT) * maxPoints;
  }
#ifdef TD_SUPPORT_BITMAP
  int32_t nBitmapBytes = (int32_t)TD_BITMAP_BYTES(maxPoints);
  spaceNeeded += (int)nBitmapBytes;
  // TODO: Currently, the compression of bitmap parts is affiliated to the column data parts, thus allocate 1 more
  // TYPE_BYTES as to comprise complete TYPE_BYTES. Otherwise, invalid read/write would be triggered.
  // spaceNeeded += TYPE_BYTES[pCol->type]; // the bitmap part is append as a single part since 2022.04.03, thus
  // remove the additional space
#endif

  if (pCol->spaceSize < spaceNeeded) {
    void *ptr = taosMemoryRealloc(pCol->pData, spaceNeeded);
    if (ptr == NULL) {
      uDebug("malloc failure, size:%" PRId64 " failed, reason:%s", (int64_t)spaceNeeded, strerror(errno));
      return -1;
    } else {
      pCol->pData = ptr;
      pCol->spaceSize = spaceNeeded;
    }
  }
#ifdef TD_SUPPORT_BITMAP

  if (IS_VAR_DATA_TYPE(pCol->type)) {
    pCol->pBitmap = POINTER_SHIFT(pCol->pData, pCol->bytes * maxPoints);
    pCol->dataOff = POINTER_SHIFT(pCol->pBitmap, nBitmapBytes);
  } else {
    pCol->pBitmap = POINTER_SHIFT(pCol->pData, pCol->bytes * maxPoints);
  }
#else
  if (IS_VAR_DATA_TYPE(pCol->type)) {
    pCol->dataOff = POINTER_SHIFT(pCol->pData, pCol->bytes * maxPoints);
  }
#endif
  return 0;
}

/**
 * Duplicate the schema and return a new object
 */
STSchema *tdDupSchema(const STSchema *pSchema) {
  int       tlen = sizeof(STSchema) + sizeof(STColumn) * schemaNCols(pSchema);
  STSchema *tSchema = (STSchema *)taosMemoryMalloc(tlen);
  if (tSchema == NULL) return NULL;

  memcpy((void *)tSchema, (void *)pSchema, tlen);

  return tSchema;
}

/**
 * Encode a schema to dst, and return the next pointer
 */
int tdEncodeSchema(void **buf, STSchema *pSchema) {
  int tlen = 0;
  tlen += taosEncodeFixedI32(buf, schemaVersion(pSchema));
  tlen += taosEncodeFixedI32(buf, schemaNCols(pSchema));

  for (int i = 0; i < schemaNCols(pSchema); i++) {
    STColumn *pCol = schemaColAt(pSchema, i);
    tlen += taosEncodeFixedI8(buf, colType(pCol));
    tlen += taosEncodeFixedI8(buf, colFlags(pCol));
    tlen += taosEncodeFixedI16(buf, colColId(pCol));
    tlen += taosEncodeFixedI16(buf, colBytes(pCol));
  }

  return tlen;
}

/**
 * Decode a schema from a binary.
 */
void *tdDecodeSchema(void *buf, STSchema **pRSchema) {
  int             version = 0;
  int             numOfCols = 0;
  STSchemaBuilder schemaBuilder;

  buf = taosDecodeFixedI32(buf, &version);
  buf = taosDecodeFixedI32(buf, &numOfCols);

  if (tdInitTSchemaBuilder(&schemaBuilder, version) < 0) return NULL;

  for (int i = 0; i < numOfCols; i++) {
    col_type_t  type = 0;
    int8_t      flags = 0;
    col_id_t    colId = 0;
    col_bytes_t bytes = 0;
    buf = taosDecodeFixedI8(buf, &type);
    buf = taosDecodeFixedI8(buf, &flags);
    buf = taosDecodeFixedI16(buf, &colId);
    buf = taosDecodeFixedI32(buf, &bytes);
    if (tdAddColToSchema(&schemaBuilder, type, flags, colId, bytes) < 0) {
      tdDestroyTSchemaBuilder(&schemaBuilder);
      return NULL;
    }
  }

  *pRSchema = tdGetSchemaFromBuilder(&schemaBuilder);
  tdDestroyTSchemaBuilder(&schemaBuilder);
  return buf;
}

int tdInitTSchemaBuilder(STSchemaBuilder *pBuilder, schema_ver_t version) {
  if (pBuilder == NULL) return -1;

  pBuilder->tCols = 256;
  pBuilder->columns = (STColumn *)taosMemoryMalloc(sizeof(STColumn) * pBuilder->tCols);
  if (pBuilder->columns == NULL) return -1;

  tdResetTSchemaBuilder(pBuilder, version);
  return 0;
}

void tdDestroyTSchemaBuilder(STSchemaBuilder *pBuilder) {
  if (pBuilder) {
    taosMemoryFreeClear(pBuilder->columns);
  }
}

void tdResetTSchemaBuilder(STSchemaBuilder *pBuilder, schema_ver_t version) {
  pBuilder->nCols = 0;
  pBuilder->tlen = 0;
  pBuilder->flen = 0;
  pBuilder->vlen = 0;
  pBuilder->version = version;
}

int32_t tdAddColToSchema(STSchemaBuilder *pBuilder, int8_t type, int8_t flags, col_id_t colId, col_bytes_t bytes) {
  if (!isValidDataType(type)) return -1;

  if (pBuilder->nCols >= pBuilder->tCols) {
    pBuilder->tCols *= 2;
    STColumn *columns = (STColumn *)taosMemoryRealloc(pBuilder->columns, sizeof(STColumn) * pBuilder->tCols);
    if (columns == NULL) return -1;
    pBuilder->columns = columns;
  }

  STColumn *pCol = &(pBuilder->columns[pBuilder->nCols]);
  colSetType(pCol, type);
  colSetColId(pCol, colId);
  colSetFlags(pCol, flags);
  if (pBuilder->nCols == 0) {
    colSetOffset(pCol, 0);
  } else {
    STColumn *pTCol = &(pBuilder->columns[pBuilder->nCols - 1]);
    colSetOffset(pCol, pTCol->offset + TYPE_BYTES[pTCol->type]);
  }

  if (IS_VAR_DATA_TYPE(type)) {
    colSetBytes(pCol, bytes);
    pBuilder->tlen += (TYPE_BYTES[type] + bytes);
    pBuilder->vlen += bytes - sizeof(VarDataLenT);
  } else {
    colSetBytes(pCol, TYPE_BYTES[type]);
    pBuilder->tlen += TYPE_BYTES[type];
    pBuilder->vlen += TYPE_BYTES[type];
  }

  pBuilder->nCols++;
  pBuilder->flen += TYPE_BYTES[type];

  ASSERT(pCol->offset < pBuilder->flen);

  return 0;
}

STSchema *tdGetSchemaFromBuilder(STSchemaBuilder *pBuilder) {
  if (pBuilder->nCols <= 0) return NULL;

  int tlen = sizeof(STSchema) + sizeof(STColumn) * pBuilder->nCols;

  STSchema *pSchema = (STSchema *)taosMemoryMalloc(tlen);
  if (pSchema == NULL) return NULL;

  schemaVersion(pSchema) = pBuilder->version;
  schemaNCols(pSchema) = pBuilder->nCols;
  schemaTLen(pSchema) = pBuilder->tlen;
  schemaFLen(pSchema) = pBuilder->flen;
  schemaVLen(pSchema) = pBuilder->vlen;

#ifdef TD_SUPPORT_BITMAP
  schemaTLen(pSchema) += (int)TD_BITMAP_BYTES(schemaNCols(pSchema));
#endif

  memcpy(schemaColAt(pSchema, 0), pBuilder->columns, sizeof(STColumn) * pBuilder->nCols);

  return pSchema;
}

void dataColInit(SDataCol *pDataCol, STColumn *pCol, int maxPoints) {
  pDataCol->type = colType(pCol);
  pDataCol->colId = colColId(pCol);
  pDataCol->bytes = colBytes(pCol);
  pDataCol->offset = colOffset(pCol) + 0;  // TD_DATA_ROW_HEAD_SIZE;

  pDataCol->len = 0;
}

static FORCE_INLINE const void *tdGetColDataOfRowUnsafe(SDataCol *pCol, int row) {
  if (IS_VAR_DATA_TYPE(pCol->type)) {
    return POINTER_SHIFT(pCol->pData, pCol->dataOff[row]);
  } else {
    return POINTER_SHIFT(pCol->pData, TYPE_BYTES[pCol->type] * row);
  }
}

bool isNEleNull(SDataCol *pCol, int nEle) {
  if (isAllRowsNull(pCol)) return true;
  for (int i = 0; i < nEle; ++i) {
    if (!isNull(tdGetColDataOfRowUnsafe(pCol, i), pCol->type)) return false;
  }
  return true;
}

void *dataColSetOffset(SDataCol *pCol, int nEle) {
  ASSERT(((pCol->type == TSDB_DATA_TYPE_BINARY) || (pCol->type == TSDB_DATA_TYPE_NCHAR)));

  void *tptr = pCol->pData;
  // char *tptr = (char *)(pCol->pData);

  VarDataOffsetT offset = 0;
  for (int i = 0; i < nEle; ++i) {
    pCol->dataOff[i] = offset;
    offset += varDataTLen(tptr);
    tptr = POINTER_SHIFT(tptr, varDataTLen(tptr));
  }
  return POINTER_SHIFT(tptr, varDataTLen(tptr));
}

SDataCols *tdNewDataCols(int maxCols, int maxRows) {
  SDataCols *pCols = (SDataCols *)taosMemoryCalloc(1, sizeof(SDataCols));
  if (pCols == NULL) {
    uDebug("malloc failure, size:%" PRId64 " failed, reason:%s", (int64_t)sizeof(SDataCols), strerror(errno));
    return NULL;
  }

  pCols->maxPoints = maxRows;
  pCols->maxCols = maxCols;
  pCols->numOfRows = 0;
  pCols->numOfCols = 0;
  pCols->bitmapMode = TSDB_BITMODE_DEFAULT;

  if (maxCols > 0) {
    pCols->cols = (SDataCol *)taosMemoryCalloc(maxCols, sizeof(SDataCol));
    if (pCols->cols == NULL) {
      uDebug("malloc failure, size:%" PRId64 " failed, reason:%s", (int64_t)sizeof(SDataCol) * maxCols,
             strerror(errno));
      tdFreeDataCols(pCols);
      return NULL;
    }
#if 0  // no need as calloc used
    int i;
    for (i = 0; i < maxCols; i++) {
      pCols->cols[i].spaceSize = 0;
      pCols->cols[i].len = 0;
      pCols->cols[i].pData = NULL;
      pCols->cols[i].dataOff = NULL;
    }
#endif
  }

  return pCols;
}

int tdInitDataCols(SDataCols *pCols, STSchema *pSchema) {
  int i;
  int oldMaxCols = pCols->maxCols;
  if (schemaNCols(pSchema) > oldMaxCols) {
    pCols->maxCols = schemaNCols(pSchema);
    void *ptr = (SDataCol *)taosMemoryRealloc(pCols->cols, sizeof(SDataCol) * pCols->maxCols);
    if (ptr == NULL) return -1;
    pCols->cols = ptr;
    for (i = oldMaxCols; i < pCols->maxCols; ++i) {
      pCols->cols[i].pData = NULL;
      pCols->cols[i].dataOff = NULL;
      pCols->cols[i].pBitmap = NULL;
      pCols->cols[i].spaceSize = 0;
    }
  }
#if 0
  tdResetDataCols(pCols); // redundant loop to reset len/blen to 0, already reset in following dataColInit(...)
#endif

  pCols->numOfRows = 0;
  pCols->bitmapMode = TSDB_BITMODE_DEFAULT;
  pCols->numOfCols = schemaNCols(pSchema);

  for (i = 0; i < schemaNCols(pSchema); ++i) {
    dataColInit(pCols->cols + i, schemaColAt(pSchema, i), pCols->maxPoints);
  }

  return 0;
}

SDataCols *tdFreeDataCols(SDataCols *pCols) {
  int i;
  if (pCols) {
    if (pCols->cols) {
      int maxCols = pCols->maxCols;
      for (i = 0; i < maxCols; ++i) {
        SDataCol *pCol = &pCols->cols[i];
        taosMemoryFreeClear(pCol->pData);
      }
      taosMemoryFree(pCols->cols);
      pCols->cols = NULL;
    }
    taosMemoryFree(pCols);
  }
  return NULL;
}

void tdResetDataCols(SDataCols *pCols) {
  if (pCols != NULL) {
    pCols->numOfRows = 0;
    pCols->bitmapMode = 0;
    for (int i = 0; i < pCols->maxCols; ++i) {
      dataColReset(pCols->cols + i);
    }
  }
}

#endif