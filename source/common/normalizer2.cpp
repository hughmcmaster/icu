/*
*******************************************************************************
*
*   Copyright (C) 2009, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
*******************************************************************************
*   file name:  normalizer2.cpp
*   encoding:   US-ASCII
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 2009nov22
*   created by: Markus W. Scherer
*/

#include "unicode/utypes.h"

#if !UCONFIG_NO_NORMALIZATION

#include "unicode/localpointer.h"
#include "unicode/normalizer2.h"
#include "unicode/udata.h"
#include "unicode/ustring.h"
#include "cmemory.h"
#include "mutex.h"
#include "normalizer2impl.h"
#include "ucln_cmn.h"
#include "utrie2.h"

U_NAMESPACE_BEGIN

// TODO: move to utrie2.h
class UTrie2StringIterator : public UMemory {
public:
    UTrie2StringIterator(const UTrie2 *t, const UChar *p) :
        trie(t), codePointStart(p), codePointLimit(p), codePoint(U_SENTINEL) {}

    const UTrie2 *trie;
    const UChar *codePointStart, *codePointLimit;
    UChar32 codePoint;
};

class BackwardUTrie2StringIterator : public UTrie2StringIterator {
public:
    BackwardUTrie2StringIterator(const UTrie2 *t, const UChar *s, const UChar *p) :
        UTrie2StringIterator(t, p), start(s) {}

    uint16_t previous16() {
        codePointLimit=codePointStart;
        if(start>=codePointStart) {
            codePoint=U_SENTINEL;
            return 0;
        }
        uint16_t result;
        UTRIE2_U16_PREV16(trie, start, codePointStart, codePoint, result);
        return result;
    }

    const UChar *start;
};

class ForwardUTrie2StringIterator : public UTrie2StringIterator {
public:
    // Iteration limit l can be NULL.
    // In that case, the caller must detect c==0 and stop.
    ForwardUTrie2StringIterator(const UTrie2 *t, const UChar *p, const UChar *l) :
        UTrie2StringIterator(t, p), limit(l) {}

    uint16_t next16() {
        codePointStart=codePointLimit;
        if(codePointLimit==limit) {
            codePoint=U_SENTINEL;
            return 0;
        }
        uint16_t result;
        UTRIE2_U16_NEXT16(trie, codePointLimit, limit, codePoint, result);
        return result;
    }

    const UChar *limit;
};

Normalizer2Data::Normalizer2Data() : memory(NULL), trie(NULL) {}

Normalizer2Data::~Normalizer2Data() {
    udata_close(memory);
    utrie2_close(trie);
}

UBool U_CALLCONV
Normalizer2Data::isAcceptable(void *context,
                              const char *type, const char *name,
                              const UDataInfo *pInfo) {
    if(
        pInfo->size>=20 &&
        pInfo->isBigEndian==U_IS_BIG_ENDIAN &&
        pInfo->charsetFamily==U_CHARSET_FAMILY &&
        pInfo->dataFormat[0]==0x4e &&    /* dataFormat="Nrm2" */
        pInfo->dataFormat[1]==0x72 &&
        pInfo->dataFormat[2]==0x6d &&
        pInfo->dataFormat[3]==0x32 &&
        pInfo->formatVersion[0]==1
    ) {
        Normalizer2Data *me=(Normalizer2Data *)context;
        uprv_memcpy(me->dataVersion, pInfo->dataVersion, 4);
        return TRUE;
    } else {
        return FALSE;
    }
}

void
Normalizer2Data::load(const char *packageName, const char *name, UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) {
        return;
    }
    memory=udata_openChoice(packageName, "nrm", name, isAcceptable, this, &errorCode);
    if(U_FAILURE(errorCode)) {
        return;
    }
    const uint8_t *inBytes=(const uint8_t *)udata_getMemory(memory);
    const int32_t *inIndexes=(const int32_t *)inBytes;
    int32_t indexesLength=inIndexes[IX_NORM_TRIE_OFFSET]/4;
    if(indexesLength<=IX_MIN_MAYBE_YES) {
        errorCode=U_INVALID_FORMAT_ERROR;  // Not enough indexes.
        return;
    }
    // Copy the indexes. Take care of possible growth of the array.
    if(indexesLength>IX_COUNT) {
        indexesLength=IX_COUNT;
    }
    uprv_memcpy(indexes, inIndexes, indexesLength*4);
    if(indexesLength<IX_COUNT) {
        uprv_memset(indexes+indexesLength, 0, (IX_COUNT-indexesLength)*4);
    }

    int32_t offset=indexes[IX_NORM_TRIE_OFFSET];
    int32_t nextOffset=indexes[IX_EXTRA_DATA_OFFSET];
    trie=utrie2_openFromSerialized(UTRIE2_16_VALUE_BITS,
                                   inBytes+offset, nextOffset-offset, NULL,
                                   &errorCode);
    if(U_FAILURE(errorCode)) {
        return;
    }

    offset=nextOffset;
    maybeYesCompositions=(const uint16_t *)(inBytes+offset);
    extraData=maybeYesCompositions+(MIN_NORMAL_MAYBE_YES-indexes[IX_MIN_MAYBE_YES]);
}

UBool ReorderingBuffer::init() {
    int32_t length=str.length();
    start=str.getBuffer(-1);
    if(start==NULL) {
        return FALSE;
    }
    limit=start+length;
    remainingCapacity=str.getCapacity()-length;
    reorderStart=start;
    if(start==limit) {
        lastCC=0;
    } else {
        setIterator();
        lastCC=previousCC();
        // Set reorderStart after the last code point with cc<=1 if there is one.
        if(lastCC>1) {
            while(previousCC()>1) {}
        }
        reorderStart=codePointLimit;
    }
    return TRUE;
}

UBool ReorderingBuffer::appendSupplementary(UChar32 c, uint8_t cc) {
    if(remainingCapacity<2 && !resize(2)) {
        return FALSE;
    }
    if(lastCC<=cc || cc==0) {
        limit[0]=U16_LEAD(c);
        limit[1]=U16_TRAIL(c);
        limit+=2;
        lastCC=cc;
        if(cc<=1) {
            reorderStart=limit;
        }
    } else {
        insert(c, cc);
    }
    remainingCapacity-=2;
    return TRUE;
}

UBool ReorderingBuffer::append(const UChar *s, int32_t length, uint8_t leadCC, uint8_t trailCC) {
    if(length==0) {
        return TRUE;
    }
    if(remainingCapacity<length && !resize(length)) {
        return FALSE;
    }
    remainingCapacity-=length;
    if(lastCC<=leadCC || leadCC==0) {
        if(trailCC<=1) {
            reorderStart=limit+length;
        } else if(leadCC<=1) {
            reorderStart=limit+1;  // Ok if not a code point boundary.
        }
        const UChar *sLimit=s+length;
        do { *limit++=*s++; } while(s!=sLimit);
        lastCC=trailCC;
    } else {
        int32_t i=0;
        UChar32 c;
        U16_NEXT(s, i, length, c);
        insert(c, leadCC);  // insert first code point
        while(i<length) {
            U16_NEXT(s, i, length, c);
            if(i<length) {
                // s must be in NFD, otherwise we need to use getCC().
                leadCC=data.getCCFromYesOrMaybe(data.getNorm16(c));
            } else {
                leadCC=trailCC;
            }
            append(c, leadCC);
        }
    }
    return TRUE;
}

UBool ReorderingBuffer::appendZeroCC(const UChar *s, int32_t length) {
    if(length==0) {
        return TRUE;
    }
    if(remainingCapacity<length && !resize(length)) {
        return FALSE;
    }
    u_memcpy(limit, s, length);
    limit+=length;
    remainingCapacity-=length;
    lastCC=0;
    reorderStart=limit;
    return TRUE;
}

void ReorderingBuffer::removeZeroCCSuffix(int32_t length) {
    if(length<(limit-start)) {
        limit-=length;
        remainingCapacity+=length;
    } else {
        limit=start;
        remainingCapacity=str.getCapacity();
    }
    reorderStart=limit;
}

UBool ReorderingBuffer::resize(int32_t appendLength) {
    int32_t reorderStartIndex=(int32_t)(reorderStart-start);
    int32_t length=(int32_t)(limit-start);
    str.releaseBuffer(length);
    int32_t newCapacity=length+appendLength;
    int32_t doubleCapacity=2*str.getCapacity();
    if(newCapacity<doubleCapacity) {
        if(doubleCapacity<1024) {
            newCapacity=1024;
        } else {
            newCapacity=doubleCapacity;
        }
    }
    start=str.getBuffer(newCapacity);
    if(start==NULL) {
        return FALSE;
    }
    reorderStart=start+reorderStartIndex;
    limit=start+length;
    remainingCapacity=str.getCapacity()-length;
    return TRUE;
}

void ReorderingBuffer::skipPrevious() {
    codePointLimit=codePointStart;
    UChar c=*--codePointStart;
    if(U16_IS_TRAIL(c) && start<codePointStart && U16_IS_LEAD(*(codePointStart-1))) {
        --codePointStart;
    }
}

uint8_t ReorderingBuffer::previousCC() {
    codePointLimit=codePointStart;
    if(reorderStart>=codePointStart) {
        return 0;
    }
    UChar c=*--codePointStart;
    if(c<Normalizer2Data::MIN_CCC_LCCC_CP) {
        return 0;
    }

    UChar c2;
    uint16_t norm16;
    if(U16_IS_TRAIL(c) && start<codePointStart && U16_IS_LEAD(c2=*(codePointStart-1))) {
        --codePointStart;
        norm16=data.getNorm16FromSurrogatePair(c2, c);
    } else {
        norm16=data.getNorm16FromBMP(c);
    }
    return data.getCCFromYesOrMaybe(norm16);
}

// Inserts c somewhere before the last character.
// Requires 0<cc<lastCC which implies reorderStart<limit.
void ReorderingBuffer::insert(UChar32 c, uint8_t cc) {
    for(setIterator(), skipPrevious(); previousCC()>cc;) {}
    // insert c at codePointLimit, after the character with prevCC<=cc
    UChar *q=limit;
    UChar *r=limit+=U16_LENGTH(c);
    do {
        *--r=*--q;
    } while(codePointLimit!=q);
    writeCodePoint(q, c);
    if(cc<=1) {
        reorderStart=r;
    }
}

Normalizer2Impl *
Normalizer2Impl::createInstance(const char *packageName,
                                const char *name,
                                UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) {
        return NULL;
    }
    LocalPointer<Normalizer2Impl> impl(new Normalizer2Impl);
    if(impl.isNull()) {
        errorCode=U_MEMORY_ALLOCATION_ERROR;
        return NULL;
    }
    impl->data.load(packageName, name, errorCode);
    return U_SUCCESS(errorCode) ? impl.orphan() : NULL;
}

U_CDECL_BEGIN
static UBool U_CALLCONV uprv_normalizer2_cleanup();
U_CDECL_END

class Normalizer2ImplSingleton : public IcuSingletonWrapper<Normalizer2Impl> {
public:
    Normalizer2ImplSingleton(IcuSingleton &s, const char *n) :
        IcuSingletonWrapper<Normalizer2Impl>(s), name(n) {}
    Normalizer2Impl *getInstance(UErrorCode &errorCode) {
        return IcuSingletonWrapper<Normalizer2Impl>::getInstance(createInstance, name, errorCode);
    }
private:
    static void *createInstance(const void *context, UErrorCode &errorCode) {
        ucln_common_registerCleanup(UCLN_COMMON_NORMALIZER2, uprv_normalizer2_cleanup);
        return Normalizer2Impl::createInstance(NULL, (const char *)context, errorCode);
    }

    const char *name;
};

STATIC_ICU_SINGLETON(nfcSingleton);
STATIC_ICU_SINGLETON(nfkcSingleton);
STATIC_ICU_SINGLETON(nfkc_cfSingleton);

U_CDECL_BEGIN

static UBool U_CALLCONV uprv_normalizer2_cleanup() {
    Normalizer2ImplSingleton(nfcSingleton, NULL).deleteInstance();
    Normalizer2ImplSingleton(nfkcSingleton, NULL).deleteInstance();
    Normalizer2ImplSingleton(nfkc_cfSingleton, NULL).deleteInstance();
    return TRUE;
}

U_CDECL_END

Normalizer2Impl *Normalizer2Impl::getNFCInstance(UErrorCode &errorCode) {
    return Normalizer2ImplSingleton(nfcSingleton, "nfc").getInstance(errorCode);
}

Normalizer2Impl *Normalizer2Impl::getNFKCInstance(UErrorCode &errorCode) {
    return Normalizer2ImplSingleton(nfkcSingleton, "nfkc").getInstance(errorCode);
}

Normalizer2Impl *Normalizer2Impl::getNFKC_CFInstance(UErrorCode &errorCode) {
    return Normalizer2ImplSingleton(nfkc_cfSingleton, "nfkc_cf").getInstance(errorCode);
}

UBool Normalizer2Impl::decompose(const UChar *src, int32_t srcLength,
                                 ReorderingBuffer &buffer) const {
    const UChar *limit;
    if(srcLength>=0) {
        limit=src+srcLength;  // string with length
    } else /* srcLength==-1 */ {
        limit=NULL;  // zero-terminated string
    }

    UChar32 minNoCP=data.getMinDecompNoCodePoint();

    U_ALIGN_CODE(16);

    for(;;) {
        // count code units below the minimum or with irrelevant data for the quick check
        UChar32 c;
        uint16_t norm16=0;
        const UChar *prevSrc=src;
        if(limit==NULL) {
            while((c=*src)<minNoCP ?
                  c!=0 : data.isMostDecompYesAndZeroCC(norm16=data.getNorm16FromSingleLead(c))) {
                ++src;
            }
        } else {
            while(src!=limit &&
                  ((c=*src)<minNoCP ||
                   data.isMostDecompYesAndZeroCC(norm16=data.getNorm16FromSingleLead(c)))) {
                ++src;
            }
        }

        // copy these code units all at once
        if(src!=prevSrc) {
            if(!buffer.appendZeroCC(prevSrc, (int32_t)(src-prevSrc))) {
                return FALSE;
            }
        }

        if(limit==NULL ? c==0 : src==limit) {
            break;  // end of source reached
        }

        // Check one above-minimum, relevant code point.
        ++src;
        UChar c2;
        if(U16_IS_LEAD(c)) {
            if(src!=limit && U16_IS_TRAIL(c2=*src)) {
                ++src;
                c=U16_GET_SUPPLEMENTARY(c, c2);
                norm16=data.getNorm16FromSupplementary(c);
            } else {
                // Data for lead surrogate code *point* not code *unit*. Normally 0.
                norm16=data.getNorm16FromBMP((UChar)c);
            }
        }
        if(!decompose(c, norm16, buffer)) {
            return FALSE;
        }
    }
    return TRUE;
}

UBool Normalizer2Impl::decompose(UChar32 c, uint16_t norm16, ReorderingBuffer &buffer) const {
    // Only loops for 1:1 algorithmic mappings.
    for(;;) {
        // get the decomposition and the lead and trail cc's
        if(data.isDecompYes(norm16)) {
            return buffer.append(c, data.getCCFromYesOrMaybe(norm16));  // c does not decompose
        } else if(data.isHangul(norm16)) {
            // Hangul syllable: decompose algorithmically
            UChar jamos[3];
            UChar c2;
            c-=HANGUL_BASE;
            c2=(UChar)(c%JAMO_T_COUNT);
            c/=JAMO_T_COUNT;
            jamos[0]=(UChar)(JAMO_L_BASE+c/JAMO_V_COUNT);
            jamos[1]=(UChar)(JAMO_V_BASE+c%JAMO_V_COUNT);
            if(c2!=0) {
                jamos[2]=(UChar)(JAMO_T_BASE+c2);
            }
            return buffer.appendZeroCC(jamos, c2==0 ? 2 : 3);
        } else if(data.isDecompNoAlgorithmic(norm16)) {
            c=data.mapAlgorithmic(c, norm16);
            norm16=data.getNorm16(c);
            continue;
        } else {
            // c decomposes, get everything from the variable-length extra data
            const uint16_t *mapping=data.getMapping(norm16);
            int32_t length=*mapping&Normalizer2Data::MAPPING_LENGTH_MASK;
            uint8_t leadCC, trailCC;
            trailCC=(uint8_t)(*mapping>>8);
            if(*mapping++&Normalizer2Data::MAPPING_HAS_CCC_LCCC_WORD) {
                leadCC=(uint8_t)(*mapping++>>8);
            } else {
                leadCC=0;
            }
            return buffer.append((const UChar *)mapping, length, leadCC, trailCC);
        }
    }
}

void Normalizer2Impl::decompose(const UChar *src, int32_t srcLength,
                                UnicodeString &dest,
                                UErrorCode &errorCode) const {
    if(U_FAILURE(errorCode)) {
        dest.setToBogus();
        return;
    }
    dest.remove();
    ReorderingBuffer buffer(data, dest);
    if(!buffer.init() || !decompose(src, srcLength, buffer)) {
        errorCode=U_MEMORY_ALLOCATION_ERROR;
        dest.setToBogus();
    }
}

void Normalizer2Impl::decomposeAndAppend(const UChar *src, int32_t srcLength,
                                         UnicodeString &dest, UBool doDecompose,
                                         UErrorCode &errorCode) const {
    if(U_FAILURE(errorCode)) {
        return;
    }
    if(dest.isBogus()) {
        errorCode=U_ILLEGAL_ARGUMENT_ERROR;
        return;
    }
    ReorderingBuffer buffer(data, dest);
    if(!buffer.init()) {
        errorCode=U_MEMORY_ALLOCATION_ERROR;
        return;
    }
    if(doDecompose) {
        if(!decompose(src, srcLength, buffer)) {
            errorCode=U_MEMORY_ALLOCATION_ERROR;
        }
        return;
    }
    // Just merge the strings at the boundary.
    if(srcLength<0) {
        srcLength=u_strlen(src);
    }
    ForwardUTrie2StringIterator iter(data.getTrie(), src, src+srcLength);
    uint16_t first16, norm16;
    first16=norm16=iter.next16();
    while(!data.isDecompYesAndZeroCC(norm16)) {
        norm16=iter.next16();
    };
    if(!buffer.append(src, (int32_t)(iter.codePointStart-src), data.getCC(first16), 0)) {
        errorCode=U_MEMORY_ALLOCATION_ERROR;
        return;
    }
    if(!buffer.appendZeroCC(iter.codePointStart, srcLength-(int32_t)(iter.codePointStart-src))) {
        errorCode=U_MEMORY_ALLOCATION_ERROR;
    }
}

/*
 * Finds the recomposition result for
 * a forward-combining "lead" character,
 * specified with a pointer to its compositions list,
 * and a backward-combining "trail" character.
 *
 * If the lead and trail characters combine, then this function returns
 * the following "compositeAndFwd" value:
 * Bits 21..1  composite character
 * Bit      0  set if the composite is a forward-combining starter
 * otherwise it returns -1.
 *
 * The compositions list has (trail, compositeAndFwd) pair entries,
 * encoded as either pairs or triples of 16-bit units.
 * The last entry has the high bit of its first unit set.
 *
 * The list is sorted by ascending trail characters (there are no duplicates).
 * A linear search is used.
 *
 * See normalizer2impl.h for a more detailed description
 * of the composition table format.
 */
static int32_t combine(const uint16_t *table, UChar32 trail) {
    uint16_t key1, firstUnit;
    if(trail<Normalizer2Data::COMP_1_TRAIL_LIMIT) {
        // trail character is 0..33FF
        // result entry may have 2 or 3 units
        key1=(uint16_t)(trail<<1);
        while(key1>(firstUnit=*table)) {
            table+=2+(firstUnit&Normalizer2Data::COMP_1_TRIPLE);
        }
        if(key1==(firstUnit&Normalizer2Data::COMP_1_TRAIL_MASK)) {
            if(firstUnit&Normalizer2Data::COMP_1_TRIPLE) {
                return ((int32_t)table[1]<<16)|table[2];
            } else {
                return table[1];
            }
        }
    } else {
        // trail character is 3400..10FFFF
        // result entry has 3 units
        key1=(uint16_t)(Normalizer2Data::COMP_1_TRAIL_LIMIT+
                        ((trail>>Normalizer2Data::COMP_1_TRAIL_SHIFT))&
                         ~Normalizer2Data::COMP_1_TRIPLE);
        uint16_t key2=(uint16_t)(trail<<Normalizer2Data::COMP_2_TRAIL_SHIFT);
        uint16_t secondUnit;
        for(;;) {
            if(key1>(firstUnit=*table)) {
                table+=2+(firstUnit&Normalizer2Data::COMP_1_TRIPLE);
            } else if(key1==(firstUnit&Normalizer2Data::COMP_1_TRAIL_MASK)) {
                if(key2>(secondUnit=table[1])) {
                    if(firstUnit&Normalizer2Data::COMP_1_LAST_TUPLE) {
                        break;
                    } else {
                        table+=3;
                    }
                } else if(key2==(secondUnit&Normalizer2Data::COMP_2_TRAIL_MASK)) {
                    return ((int32_t)(secondUnit&~Normalizer2Data::COMP_2_TRAIL_MASK)<<16)|table[2];
                } else {
                    break;
                }
            }
        }
    }
    return -1;
}

UBool Normalizer2Impl::compose(const UChar *src, int32_t srcLength,
                               ReorderingBuffer &buffer) const {
    const UChar *limit;
    if(srcLength>=0) {
        limit=src+srcLength;  // string with length
    } else /* srcLength==-1 */ {
        limit=NULL;  // zero-terminated string
    }

    UChar32 minNoMaybeCP=data.getMinCompNoMaybeCodePoint();

    U_ALIGN_CODE(16);

    // *** TODO: The rest of this function is just a copy of decompose() so far. ***

    for(;;) {
        // count code units below the minimum or with irrelevant data for the quick check
        UChar32 c;
        uint16_t norm16=0;
        const UChar *prevSrc=src;
        if(limit==NULL) {
            while((c=*src)<minNoMaybeCP ?
                  c!=0 : data.isMostDecompYesAndZeroCC(norm16=data.getNorm16FromSingleLead(c))) {
                ++src;
            }
        } else {
            while(src!=limit &&
                  ((c=*src)<minNoMaybeCP ||
                   data.isMostDecompYesAndZeroCC(norm16=data.getNorm16FromSingleLead(c)))) {
                ++src;
            }
        }

        // copy these code units all at once
        if(src!=prevSrc) {
            if(!buffer.appendZeroCC(prevSrc, (int32_t)(src-prevSrc))) {
                return FALSE;
            }
        }

        if(limit==NULL ? c==0 : src==limit) {
            break;  // end of source reached
        }

        // Check one above-minimum, relevant code point.
        ++src;
        UChar c2;
        if(U16_IS_LEAD(c)) {
            if(src!=limit && U16_IS_TRAIL(c2=*src)) {
                ++src;
                c=U16_GET_SUPPLEMENTARY(c, c2);
                norm16=data.getNorm16FromSupplementary(c);
            } else {
                // Data for lead surrogate code *point* not code *unit*. Normally 0.
                norm16=data.getNorm16FromBMP((UChar)c);
            }
        }
        if(!decompose(c, norm16, buffer)) {
            return FALSE;
        }
    }
    return TRUE;
}

void Normalizer2Impl::compose(const UChar *src, int32_t srcLength,
                              UnicodeString &dest,
                              UErrorCode &errorCode) const {
    if(U_FAILURE(errorCode)) {
        dest.setToBogus();
        return;
    }
    dest.remove();
    ReorderingBuffer buffer(data, dest);
    if(!buffer.init() || !compose(src, srcLength, buffer)) {
        errorCode=U_MEMORY_ALLOCATION_ERROR;
        dest.setToBogus();
    }
}

void Normalizer2Impl::composeAndAppend(const UChar *src, int32_t srcLength,
                                       UnicodeString &dest, UBool doCompose,
                                       UErrorCode &errorCode) const {
    if(U_FAILURE(errorCode)) {
        return;
    }
    if(dest.isBogus()) {
        errorCode=U_ILLEGAL_ARGUMENT_ERROR;
        return;
    }
    ReorderingBuffer buffer(data, dest);
    if(!buffer.init()) {
        errorCode=U_MEMORY_ALLOCATION_ERROR;
        return;
    }
    if(!buffer.isEmpty()) {
        const UChar *firstStarterInSrc=findNextCompStarter(src,
                                                           srcLength>=0 ? src+srcLength : NULL);
        if(src!=firstStarterInSrc) {
            const UChar *lastStarterInDest=findPreviousCompStarter(buffer.getStart(),
                                                                   buffer.getLimit());
            UnicodeString middle(lastStarterInDest,
                                 (int32_t)(buffer.getLimit()-lastStarterInDest));
            buffer.removeZeroCCSuffix((int32_t)(buffer.getLimit()-lastStarterInDest));
            middle.append(src, (int32_t)(firstStarterInSrc-src));
            if(!compose(middle.getBuffer(), middle.length(), buffer)) {
                errorCode=U_MEMORY_ALLOCATION_ERROR;
                return;
            }
            if(srcLength>=0) {
                srcLength-=(int32_t)(firstStarterInSrc-src);
            }
            src=firstStarterInSrc;
        }
    }
    if(doCompose ?
            !compose(src, srcLength, buffer) :
            !buffer.appendZeroCC(src, srcLength>=0 ? srcLength : u_strlen(src))) {
        errorCode=U_MEMORY_ALLOCATION_ERROR;
    }
}

UBool Normalizer2Impl::isCompStarter(UChar32 c, uint16_t norm16) const {
    // Partial copy of the decompose(c) function.
    for(;;) {
        if(data.isCompYesAndZeroCC(norm16)) {
            return TRUE;
        } else if(data.isMaybeOrNonZeroCC(norm16)) {
            return FALSE;
        } else if(data.isDecompNoAlgorithmic(norm16)) {
            c=data.mapAlgorithmic(c, norm16);
            norm16=data.getNorm16(c);
            continue;
        } else {
            // c decomposes, get everything from the variable-length extra data
            const uint16_t *mapping=data.getMapping(norm16);
            int32_t length=*mapping&Normalizer2Data::MAPPING_LENGTH_MASK;
            if((*mapping++&Normalizer2Data::MAPPING_HAS_CCC_LCCC_WORD) && (*mapping++&0xff00)) {
                return FALSE;  // non-zero leadCC
            }
            if(length==0) {
                return FALSE;
            }
            int32_t i=0;
            UChar32 c;
            U16_NEXT_UNSAFE(mapping, i, c);
            return data.isCompYesAndZeroCC(data.getNorm16(c));
        }
    }
}

const UChar *Normalizer2Impl::findPreviousCompStarter(const UChar *start, const UChar *p) const {
    BackwardUTrie2StringIterator iter(data.getTrie(), start, p);
    uint16_t norm16;
    do {
        norm16=iter.previous16();
    } while(!isCompStarter(iter.codePoint, norm16));
    return iter.codePointStart;
}

const UChar *Normalizer2Impl::findNextCompStarter(const UChar *p, const UChar *limit) const {
    ForwardUTrie2StringIterator iter(data.getTrie(), p, limit);
    uint16_t norm16;
    do {
        norm16=iter.next16();
    } while(!isCompStarter(iter.codePoint, norm16));
    return iter.codePointStart;
}

// Public API dispatch via Normalizer2 subclasses -------------------------- ***

UnicodeString &
DecomposeNormalizer2::normalize(const UnicodeString &src,
                                UnicodeString &dest,
                                UErrorCode &errorCode) const {
    assertNotBogus(src, errorCode);
    impl.decompose(src.getBuffer(), src.length(), dest, errorCode);
    return dest;
}

UnicodeString &
DecomposeNormalizer2::normalizeSecondAndAppend(UnicodeString &first,
                                               const UnicodeString &second,
                                               UErrorCode &errorCode) const {
    assertNotBogus(second, errorCode);
    impl.decomposeAndAppend(second.getBuffer(), second.length(), first, TRUE, errorCode);
    return first;
}

UnicodeString &
DecomposeNormalizer2::append(UnicodeString &first,
                             const UnicodeString &second,
                             UErrorCode &errorCode) const {
    assertNotBogus(second, errorCode);
    impl.decomposeAndAppend(second.getBuffer(), second.length(), first, FALSE, errorCode);
    return first;
}

UnicodeString &
ComposeNormalizer2::normalize(const UnicodeString &src,
                              UnicodeString &dest,
                              UErrorCode &errorCode) const {
    assertNotBogus(src, errorCode);
    impl.compose(src.getBuffer(), src.length(), dest, errorCode);
    return dest;
}

UnicodeString &
ComposeNormalizer2::normalizeSecondAndAppend(UnicodeString &first,
                                             const UnicodeString &second,
                                             UErrorCode &errorCode) const {
    assertNotBogus(second, errorCode);
    impl.composeAndAppend(second.getBuffer(), second.length(), first, TRUE, errorCode);
    return first;
}

UnicodeString &
ComposeNormalizer2::append(UnicodeString &first,
                           const UnicodeString &second,
                           UErrorCode &errorCode) const {
    assertNotBogus(second, errorCode);
    impl.composeAndAppend(second.getBuffer(), second.length(), first, FALSE, errorCode);
    return first;
}

U_NAMESPACE_END

#endif  // !UCONFIG_NO_NORMALIZATION

// #if for code only for minMaybeYes<MIN_NORMAL_MAYBE_YES. Benchmark both ways.
// Consider not supporting an exclusions set at runtime.
//   Otherwise need to pull nx_contains() into the ReorderingBuffer etc.
//   Can support Unicode 3.2 normalization via UnicodeSet span outside of normalization calls.
