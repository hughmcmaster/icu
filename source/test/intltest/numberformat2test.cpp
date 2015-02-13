/*
*******************************************************************************
* Copyright (C) 2015, International Business Machines Corporation and         *
* others. All Rights Reserved.                                                *
*******************************************************************************
*
* File NUMBERFORMAT2TEST.CPP
*
*******************************************************************************
*/
#include "unicode/utypes.h"

#include "intltest.h"

#if !UCONFIG_NO_FORMATTING

#include "digitformatter.h"
#include "sciformatter.h"
#include "digitinterval.h"
#include "significantdigitinterval.h"
#include "digitlst.h"
#include "digitgrouping.h"
#include "unicode/localpointer.h"
#include "fphdlimp.h"
#include "digitaffixesandpadding.h"
#include "valueformatter.h"
#include "precision.h"
#include "plurrule_impl.h"
#include "unicode/plurrule.h"
#include "decimalformatpattern.h"
#include "affixpatternparser.h"
#include "datadrivennumberformattestsuite.h"
#include "charstr.h"

static const int32_t kIntField = 4938;
static const int32_t kSignField = 5770;

struct NumberFormat2Test_Attributes {
    int32_t id;
    int32_t spos;
    int32_t epos;
};

class NumberFormat2Test_FieldPositionHandler : public FieldPositionHandler {
public:
NumberFormat2Test_Attributes attributes[100];
int32_t count;
UBool bRecording;



NumberFormat2Test_FieldPositionHandler() : count(0), bRecording(TRUE) { attributes[0].spos = -1; }
NumberFormat2Test_FieldPositionHandler(UBool recording) : count(0), bRecording(recording) { attributes[0].spos = -1; }
virtual ~NumberFormat2Test_FieldPositionHandler();
virtual void addAttribute(int32_t id, int32_t start, int32_t limit);
virtual void shiftLast(int32_t delta);
virtual UBool isRecording(void) const;
};
 
NumberFormat2Test_FieldPositionHandler::~NumberFormat2Test_FieldPositionHandler() {
}

void NumberFormat2Test_FieldPositionHandler::addAttribute(
        int32_t id, int32_t start, int32_t limit) {
    if (count == UPRV_LENGTHOF(attributes) - 1) {
        return;
    }
    attributes[count].id = id;
    attributes[count].spos = start;
    attributes[count].epos = limit;
    ++count;
    attributes[count].spos = -1;
}

void NumberFormat2Test_FieldPositionHandler::shiftLast(int32_t /* delta */) {
}

UBool NumberFormat2Test_FieldPositionHandler::isRecording() const {
    return bRecording;
}

// Map output from pattern parser to what we actually need for scientific
// notation.
static void fixScientificExponentGrouping(
        const FixedPrecision &original, FixedPrecision &newResult) {
    int32_t maxIntDigitCount = original.fMax.getIntDigitCount();
    int32_t minIntDigitCount = original.fMin.getIntDigitCount();
    int32_t maxFracDigitCount = original.fMax.getFracDigitCount();
    int32_t minFracDigitCount = original.fMin.getFracDigitCount();

    // Per the spec, exponent grouping happens if maxIntDigitCount is more
    // than 1 and more than minIntDigitCount.
    UBool bExponentGrouping = maxIntDigitCount > 1 && minIntDigitCount < maxIntDigitCount;
    if (bExponentGrouping) {
        newResult.fMax.setIntDigitCount(maxIntDigitCount);

        // For exponent grouping minIntDigits is always treated as 1 even
        // if it wasn't set to 1!
        newResult.fMin.setIntDigitCount(1);
    } else {
        // Fixed digit count left of decimal. minIntDigitCount doesn't have
        // to equal maxIntDigitCount i.e minIntDigitCount == 0 while
        // maxIntDigitCount == 1.
        int32_t fixedIntDigitCount = maxIntDigitCount;

        // If fixedIntDigitCount is 0 but 
        // either fraction count is 0 too then use 1. This way we can get
        // unlimited precision for X.XXXEX
        if (fixedIntDigitCount == 0 && (maxFracDigitCount == 0 || minFracDigitCount == 0)) {
            fixedIntDigitCount = 1;
        }
        newResult.fMax.setIntDigitCount(fixedIntDigitCount);
        newResult.fMin.setIntDigitCount(fixedIntDigitCount);
    }
    // Spec says this is how we compute significant digits. 0 means
    // unlimited significant digits.
    int32_t maxSigDigits = minIntDigitCount + maxFracDigitCount;
    if (maxSigDigits > 0) {
        int32_t minSigDigits = minIntDigitCount + minFracDigitCount;
        newResult.fSignificant.setMin(minSigDigits);
        newResult.fSignificant.setMax(maxSigDigits);
    }
    newResult.fRoundingIncrement = original.fRoundingIncrement;
}

class NumberFormat2TestDecimalFormat : public UMemory {
public:
ScientificPrecision fPrecision;
SciFormatterOptions fOptions;
DigitGrouping fGrouping;
SciFormatter fSciFormatter;
DigitFormatter fFormatter;
AffixPatternParser fAffixParser;
DigitAffixesAndPadding fAap;
AffixPattern fPositivePrefixPattern;
AffixPattern fNegativePrefixPattern;
AffixPattern fPositiveSuffixPattern;
AffixPattern fNegativeSuffixPattern;
UBool fUseScientific;
Locale fLocale;
PluralRules *fRules;
int32_t fScale;

NumberFormat2TestDecimalFormat(
        const DecimalFormatSymbols &sym,
        const UnicodeString &pattern, UErrorCode &status);
~NumberFormat2TestDecimalFormat();
ValueFormatter &prepareValueFormatter(
        ValueFormatter &vf, ScientificPrecision &scratchPrecision);
UnicodeString &format(
        DigitList &dl, UnicodeString &appendTo, UErrorCode &status);
void setCurrency(const UChar *currency, UErrorCode &status);
UBool usesCurrency();
int32_t parseAffixes(
        const UChar *currency, UErrorCode &status);
void parsePattern(
        const UnicodeString &pattern, UErrorCode &status);
};


NumberFormat2TestDecimalFormat::NumberFormat2TestDecimalFormat(
        const DecimalFormatSymbols &sym, 
        const UnicodeString &pattern, UErrorCode &status) :
        fSciFormatter(sym), fFormatter(sym), fAffixParser(sym),
        fRules(NULL) {
    parsePattern(pattern, status);
    if (U_FAILURE(status)) {
        return;
    }
    fLocale = sym.getLocale();
    fScale = parseAffixes(NULL, status);
}

NumberFormat2TestDecimalFormat::~NumberFormat2TestDecimalFormat() {
    delete fRules;
}

ValueFormatter &
NumberFormat2TestDecimalFormat::prepareValueFormatter(
        ValueFormatter &vf, ScientificPrecision &scratchPrecision) {
    if (fUseScientific) {
        fixScientificExponentGrouping(fPrecision.fMantissa, scratchPrecision.fMantissa);
        vf.prepareScientificFormatting(
                fSciFormatter, fFormatter, scratchPrecision, fOptions);
        return vf;
    }
    vf.prepareFixedDecimalFormatting(
            fFormatter, fGrouping, fPrecision.fMantissa, fOptions.fMantissa);
    return vf;
}

UnicodeString &
NumberFormat2TestDecimalFormat::format(
        DigitList &dl, UnicodeString &appendTo, UErrorCode &status) {
    dl.shiftDecimalRight(fScale);
    FieldPosition fpos(FieldPosition::DONT_CARE);
    FieldPositionOnlyHandler handler(fpos);
    ValueFormatter vf;
    ScientificPrecision scratch;
    return fAap.format(
            dl,
            prepareValueFormatter(vf, scratch),
            handler,
            fRules,
            appendTo,
            status);
}


void NumberFormat2TestDecimalFormat::setCurrency(
        const UChar *currency, UErrorCode &status) {
    if (usesCurrency()) {
        if (currency == NULL || currency[0] == 0) {
            parseAffixes(NULL, status);
        } else {
            UChar theCurrency[4];
            u_strncpy(theCurrency, currency, 3);
            theCurrency[3] = 0;
            parseAffixes(theCurrency, status);
        }
    }
}

UBool NumberFormat2TestDecimalFormat::usesCurrency() {
    return fPositivePrefixPattern.usesCurrency()
    || fNegativePrefixPattern.usesCurrency()
    || fPositiveSuffixPattern.usesCurrency()
    || fNegativeSuffixPattern.usesCurrency();
}

#define NUMBERFORMAT2TEST_UPDATE(dest, src) ((dest) = (dest) == 0 ? (src) : (dest))

int32_t NumberFormat2TestDecimalFormat::parseAffixes(
        const UChar *currency, UErrorCode &status) {
    if (U_FAILURE(status)) {
        return 0;
    }
    if (usesCurrency()) {
        if (!fRules) {
            fRules = PluralRules::forLocale(fLocale, status);
            if (U_FAILURE(status)) {
                return 0;
            }
        }
        UChar currencyBuf[4];
        if (currency == NULL) {
            ucurr_forLocale(fLocale.getName(), currencyBuf, UPRV_LENGTHOF(currencyBuf), &status);
            if (U_SUCCESS(status)) {
                currency = currencyBuf;
            } else {
                status = U_ZERO_ERROR;
            }
        }
        fAffixParser.fCurrencyAffixInfo.set(
                fLocale.getName(), fRules, currency, status);
        if (currency) {
            CurrencyAffixInfo::adjustPrecision(
                    currency, UCURR_USAGE_STANDARD,
                    fPrecision.fMantissa, status);
        }
    }
    int32_t result = 0;
    fAap.fPositivePrefix.remove();
    fAap.fNegativePrefix.remove();
    fAap.fPositiveSuffix.remove();
    fAap.fNegativeSuffix.remove();
    NUMBERFORMAT2TEST_UPDATE(result, fAffixParser.parse(fPositivePrefixPattern, fAap.fPositivePrefix, status));
    NUMBERFORMAT2TEST_UPDATE(result, fAffixParser.parse(fNegativePrefixPattern, fAap.fNegativePrefix, status));
    NUMBERFORMAT2TEST_UPDATE(result, fAffixParser.parse(fPositiveSuffixPattern, fAap.fPositiveSuffix, status));
    NUMBERFORMAT2TEST_UPDATE(result, fAffixParser.parse(fNegativeSuffixPattern, fAap.fNegativeSuffix, status));
    return result;
}

void NumberFormat2TestDecimalFormat::parsePattern(
        const UnicodeString &pattern, UErrorCode &status) {
    if (U_FAILURE(status)) {
        return;
    }
    DecimalFormatPatternParser patternParser;
    UParseError perror;
    DecimalFormatPattern out;
    patternParser.applyPatternWithoutExpandAffix(
            pattern, out, perror, status);
    if (U_FAILURE(status)) {
        return;
    }
    fUseScientific = out.fUseExponentialNotation;
    if (out.fUseSignificantDigits) {
        fPrecision.fMantissa.fSignificant.setMin(out.fMinimumSignificantDigits);
        fPrecision.fMantissa.fSignificant.setMax(out.fMaximumSignificantDigits);
    } else {
        fPrecision.fMantissa.fMin.setIntDigitCount(out.fMinimumIntegerDigits);
        fPrecision.fMantissa.fMax.setIntDigitCount(out.fMaximumIntegerDigits);
        fPrecision.fMantissa.fMin.setFracDigitCount(out.fMinimumFractionDigits);
        fPrecision.fMantissa.fMax.setFracDigitCount(out.fMaximumFractionDigits);
    }
    fOptions.fExponent.fMinDigits = out.fMinExponentDigits;
    fOptions.fExponent.fAlwaysShowSign = out.fExponentSignAlwaysShown;
    if (out.fGroupingUsed) {
        fGrouping.fGrouping = out.fGroupingSize;
        fGrouping.fGrouping2 = out.fGroupingSize2;
    }
    fOptions.fMantissa.fAlwaysShowDecimal = out.fDecimalSeparatorAlwaysShown;
    if (out.fRoundingIncrementUsed) {
        fPrecision.fMantissa.fRoundingIncrement = out.fRoundingIncrement;
    }
    fAap.fPadChar = out.fPad;
    fNegativePrefixPattern.remove();
    fNegativeSuffixPattern.remove();
    fPositivePrefixPattern.remove();
    fPositiveSuffixPattern.remove();
    AffixPattern::parseAffixString(
            out.fNegPrefixPattern,
            fNegativePrefixPattern,
            status);
    AffixPattern::parseAffixString(
            out.fNegSuffixPattern,
            fNegativeSuffixPattern,
            status);
    AffixPattern::parseAffixString(
            out.fPosPrefixPattern,
            fPositivePrefixPattern,
            status);
    AffixPattern::parseAffixString(
            out.fPosSuffixPattern,
            fPositiveSuffixPattern,
            status);
    fAap.fWidth = out.fFormatWidth;
    fAap.fWidth += fPositivePrefixPattern.countChar32();
    fAap.fWidth += fPositiveSuffixPattern.countChar32();
    switch (out.fPadPosition) {
    case DecimalFormatPattern::kPadBeforePrefix:
        fAap.fPadPosition = DigitAffixesAndPadding::kPadBeforePrefix;
        break;    
    case DecimalFormatPattern::kPadAfterPrefix:
        fAap.fPadPosition = DigitAffixesAndPadding::kPadAfterPrefix;
        break;    
    case DecimalFormatPattern::kPadBeforeSuffix:
        fAap.fPadPosition = DigitAffixesAndPadding::kPadBeforeSuffix;
        break;    
    case DecimalFormatPattern::kPadAfterSuffix:
        fAap.fPadPosition = DigitAffixesAndPadding::kPadAfterSuffix;
        break;    
    default:
        break;
    }
}

class NumberFormat2TestDataDriven : public icu::DataDrivenNumberFormatTestSuite {
public:
NumberFormat2TestDataDriven(IntlTest *t) : DataDrivenNumberFormatTestSuite(t) { }
protected:
UBool isFormatPass(
        const NumberFormatTestTuple &tuple,
        UnicodeString &appendErrorMessage,
        UErrorCode &status);
private:

};


UBool NumberFormat2TestDataDriven::isFormatPass(
        const NumberFormatTestTuple &tuple,
        UnicodeString &appendErrorMessage,
        UErrorCode &status) {
    if (U_FAILURE(status)) {
        return FALSE;
    }
    Locale en("en");
    DecimalFormatSymbols symbols(NFTT_GET_FIELD(tuple, locale, en), status);
    NumberFormat2TestDecimalFormat fmt(
            symbols, NFTT_GET_FIELD(tuple, pattern, "0"), status);
    if (U_FAILURE(status)) {
        appendErrorMessage.append("Error creating DecimalFormat");
        return FALSE;
    }
    if (tuple.minIntegerDigitsFlag) {
        fmt.fPrecision.fMantissa.fMin.setIntDigitCount(
                tuple.minIntegerDigits < 0 ? 0 : tuple.minIntegerDigits);
    }
    if (tuple.maxIntegerDigitsFlag) {
        fmt.fPrecision.fMantissa.fMax.setIntDigitCount(
                tuple.maxIntegerDigits < 0 ? 0 : tuple.maxIntegerDigits);
    }
    if (tuple.minFractionDigitsFlag) {
        fmt.fPrecision.fMantissa.fMin.setFracDigitCount(
                tuple.minFractionDigits < 0 ? 0 : tuple.minFractionDigits);
    }
    if (tuple.maxFractionDigitsFlag) {
        fmt.fPrecision.fMantissa.fMax.setFracDigitCount(
                tuple.maxFractionDigits < 0 ? 0 : tuple.maxFractionDigits);
    }
    if (tuple.currencyFlag) {
        UnicodeString currency(tuple.currency);
        fmt.setCurrency(currency.getTerminatedBuffer(), status);
        if (U_FAILURE(status)) {
            appendErrorMessage.append("Error setting currency.");
            return FALSE;
        }
    }
    if (tuple.minGroupingDigitsFlag) {
        fmt.fGrouping.fMinGrouping = tuple.minGroupingDigits;
    }
    UnicodeString appendTo;
    CharString formatValue;
    formatValue.appendInvariantChars(tuple.format, status);
    DigitList digitList;
    digitList.set(StringPiece(formatValue.data()), status, 0);
    digitList.reduce();
    fmt.format(digitList, appendTo, status);
    if (U_FAILURE(status)) {
        appendErrorMessage.append("Error formatting.");
        return FALSE;
    }
    if (appendTo != tuple.output) {
        appendErrorMessage.append(
                UnicodeString("Expected: ") + tuple.output + ", got: " + appendTo);
        return FALSE;
    }
    return TRUE;
}


class NumberFormat2Test : public IntlTest {
public:
    void runIndexedTest(int32_t index, UBool exec, const char *&name, char *par=0);
private:
    void TestQuantize();
    void TestConvertScientificNotation();
    void TestRounding();
    void TestRoundingIncrement();
    void TestDigitInterval();
    void verifyInterval(const DigitInterval &, int32_t minInclusive, int32_t maxExclusive);
    void TestGroupingUsed();
    void TestBenchmark();
    void TestBenchmark2();
    void TestDigitListInterval();
    void TestDigitAffixesAndPadding();
    void TestPluralsAndRounding();
    void TestPluralsAndRoundingScientific();
    void TestValueFormatter();
    void TestValueFormatterScientific();
    void TestCurrencyAffixInfo();
    void TestAffixPatternParser();
    void TestPluralAffix();
    void TestDigitAffix();
    void TestDigitFormatterDefaultCtor();
    void TestDigitIntFormatter();
    void TestDigitFormatter();
    void TestSciFormatterDefaultCtor();
    void TestSciFormatter();
    void TestDigitListToFixedDecimal();
    void TestDataDriven();
    void verifyFixedDecimal(
            const FixedDecimal &result,
            int64_t numerator,
            int64_t denominator,
            UBool bNegative,
            int32_t v,
            int64_t f);
    void verifyAffix(
            const UnicodeString &expected,
            const DigitAffix &affix,
            const NumberFormat2Test_Attributes *expectedAttributes);
    void verifyValueFormatter(
            const UnicodeString &expected,
            const ValueFormatter &formatter,
            DigitList &digits,
            const NumberFormat2Test_Attributes *expectedAttributes);
    void verifyAffixesAndPadding(
            const UnicodeString &expected,
            const DigitAffixesAndPadding &aaf,
            DigitList &digits,
            const ValueFormatter &vf,
            const PluralRules *optPluralRules,
            const NumberFormat2Test_Attributes *expectedAttributes);
    void verifyDigitList(
        const UnicodeString &expected,
        const DigitList &digits);
    void verifyDigitIntFormatter(
            const UnicodeString &expected,
            const DigitFormatter &formatter,
            int32_t value,
            int32_t minDigits,
            UBool alwaysShowSign,
            const NumberFormat2Test_Attributes *expectedAttributes);
    void verifyDigitFormatter(
            const UnicodeString &expected,
            const DigitFormatter &formatter,
            const DigitList &digits,
            const DigitGrouping &grouping,
            const DigitInterval &interval,
            UBool alwaysShowDecimal,
            const NumberFormat2Test_Attributes *expectedAttributes);
    void verifySciFormatter(
            const UnicodeString &expected,
            const SciFormatter &sciformatter,
            const DigitList &mantissa,
            int32_t exponent,
            const DigitFormatter &formatter,
            const DigitInterval &interval,
            const SciFormatterOptions &options,
            const NumberFormat2Test_Attributes *expectedAttributes);
    void verifyAttributes(
            const NumberFormat2Test_Attributes *expected,
            const NumberFormat2Test_Attributes *actual);
};

void NumberFormat2Test::runIndexedTest(
        int32_t index, UBool exec, const char *&name, char *) {
    if (exec) {
        logln("TestSuite ScientificNumberFormatterTest: ");
    }
    TESTCASE_AUTO_BEGIN;
    TESTCASE_AUTO(TestQuantize);
    TESTCASE_AUTO(TestConvertScientificNotation);
    TESTCASE_AUTO(TestRounding);
    TESTCASE_AUTO(TestRoundingIncrement);
    TESTCASE_AUTO(TestDigitInterval);
    TESTCASE_AUTO(TestGroupingUsed);
    TESTCASE_AUTO(TestDigitListInterval);
    TESTCASE_AUTO(TestDigitFormatterDefaultCtor);
    TESTCASE_AUTO(TestDigitIntFormatter);
    TESTCASE_AUTO(TestDigitFormatter);
    TESTCASE_AUTO(TestSciFormatterDefaultCtor);
    TESTCASE_AUTO(TestSciFormatter);
    TESTCASE_AUTO(TestBenchmark);
    TESTCASE_AUTO(TestBenchmark2);
    TESTCASE_AUTO(TestCurrencyAffixInfo);
    TESTCASE_AUTO(TestAffixPatternParser);
    TESTCASE_AUTO(TestPluralAffix);
    TESTCASE_AUTO(TestDigitAffix);
    TESTCASE_AUTO(TestValueFormatter);
    TESTCASE_AUTO(TestValueFormatterScientific);
    TESTCASE_AUTO(TestDigitAffixesAndPadding);
    TESTCASE_AUTO(TestPluralsAndRounding);
    TESTCASE_AUTO(TestPluralsAndRoundingScientific);
    TESTCASE_AUTO(TestDigitListToFixedDecimal);
    TESTCASE_AUTO(TestDataDriven);
 
    TESTCASE_AUTO_END;
}

void NumberFormat2Test::TestDigitInterval() {
    DigitInterval all;
    DigitInterval threeInts;
    DigitInterval fourFrac;
    threeInts.setIntDigitCount(3);
    fourFrac.setFracDigitCount(4);
    verifyInterval(all, INT32_MIN, INT32_MAX);
    verifyInterval(threeInts, INT32_MIN, 3);
    verifyInterval(fourFrac, -4, INT32_MAX);
    {
        DigitInterval result(threeInts);
        result.shrinkToFitWithin(fourFrac);
        verifyInterval(result, -4, 3);
        assertEquals("", 7, result.length());
    }
    {
        DigitInterval result(threeInts);
        result.expandToContain(fourFrac);
        verifyInterval(result, INT32_MIN, INT32_MAX);
    }
    {
        DigitInterval result(threeInts);
        result.setIntDigitCount(0);
        verifyInterval(result, INT32_MIN, 0);
        result.setIntDigitCount(-1);
        verifyInterval(result, INT32_MIN, INT32_MAX);
    }
    {
        DigitInterval result(fourFrac);
        result.setFracDigitCount(0);
        verifyInterval(result, 0, INT32_MAX);
        result.setFracDigitCount(-1);
        verifyInterval(result, INT32_MIN, INT32_MAX);
    }
}

void NumberFormat2Test::verifyInterval(
        const DigitInterval &interval,
        int32_t minInclusive, int32_t maxExclusive) {
    assertEquals("", minInclusive, interval.getLeastSignificantInclusive());
    assertEquals("", maxExclusive, interval.getMostSignificantExclusive());
    assertEquals("", maxExclusive, interval.getIntDigitCount());
}

void NumberFormat2Test::TestGroupingUsed() {
    {
        DigitGrouping grouping;
        assertFalse("", grouping.isGroupingUsed());
    }
    {
        DigitGrouping grouping;
        grouping.fGrouping = 2;
        assertTrue("", grouping.isGroupingUsed());
    }
}

void NumberFormat2Test::TestDigitListInterval() {
    DigitInterval result;
    DigitList digitList;
    {
        digitList.set(12345);
        verifyInterval(digitList.getSmallestInterval(result), 0, 5);
    }
    {
        digitList.set(1000.00);
        verifyInterval(digitList.getSmallestInterval(result), 0, 4);
    }
    {
        digitList.set(43.125);
        verifyInterval(digitList.getSmallestInterval(result), -3, 2);
    }
    {
        digitList.set(.0078125);
        verifyInterval(digitList.getSmallestInterval(result), -7, 0);
    }
    {
        // Smallest interval already has 4 significant digits
        digitList.set(1000.00);
        verifyInterval(digitList.getSmallestInterval(result, 4), 0, 4);
    }
    {
        // Smallest interval needs to expand to have 5 significant digits
        digitList.set(1000.00);
        verifyInterval(digitList.getSmallestInterval(result, 5), -1, 4);
    }
    {
        digitList.set(43.125);
        verifyInterval(digitList.getSmallestInterval(result, 5), -3, 2);
    }
    {
        digitList.set(43.125);
        verifyInterval(digitList.getSmallestInterval(result, 7), -5, 2);
    }
    {
        digitList.set(.0078125);
        verifyInterval(digitList.getSmallestInterval(result, 7), -7, 0);
    }
    {
        digitList.set(.0078125);
        verifyInterval(digitList.getSmallestInterval(result, 8), -8, 0);
    }
}

void NumberFormat2Test::TestQuantize() {
    DigitList quantity;
    quantity.set(0.00168);
    quantity.roundAtExponent(-5);
    DigitList digits;
    UErrorCode status = U_ZERO_ERROR;
    {
        digits.set(1);
        digits.quantize(quantity, status);
        verifyDigitList(".9996", digits);
    }
    {
        // round half even up
        digits.set(1.00044);
        digits.roundAtExponent(-5);
        digits.quantize(quantity, status);
        verifyDigitList("1.00128", digits);
    }
    {
        // round half down
        digits.set(0.99876);
        digits.roundAtExponent(-5);
        digits.quantize(quantity, status);
        verifyDigitList(".99792", digits);
    }
}

void NumberFormat2Test::TestConvertScientificNotation() {
    DigitList digits;
    {
        digits.set(186283);
        assertEquals("", 5, digits.toScientific(1, 1));
        verifyDigitList(
                "1.86283",
                digits);
    }
    {
        digits.set(186283);
        assertEquals("", 0, digits.toScientific(6, 1));
        verifyDigitList(
                "186283",
                digits);
    }
    {
        digits.set(186283);
        assertEquals("", -2, digits.toScientific(8, 1));
        verifyDigitList(
                "18628300",
                digits);
    }
    {
        digits.set(43561);
        assertEquals("", 6, digits.toScientific(-1, 3));
        verifyDigitList(
                ".043561",
                digits);
    }
    {
        digits.set(43561);
        assertEquals("", 3, digits.toScientific(0, 3));
        verifyDigitList(
                "43.561",
                digits);
    }
    {
        digits.set(43561);
        assertEquals("", 3, digits.toScientific(2, 3));
        verifyDigitList(
                "43.561",
                digits);
    }
    {
        digits.set(43561);
        assertEquals("", 0, digits.toScientific(3, 3));
        verifyDigitList(
                "43561",
                digits);
    }
    {
        digits.set(43561);
        assertEquals("", 0, digits.toScientific(5, 3));
        verifyDigitList(
                "43561",
                digits);
    }
    {
        digits.set(43561);
        assertEquals("", -3, digits.toScientific(6, 3));
        verifyDigitList(
                "43561000",
                digits);
    }
    {
        digits.set(43561);
        assertEquals("", -3, digits.toScientific(8, 3));
        verifyDigitList(
                "43561000",
                digits);
    }
    {
        digits.set(43561);
        assertEquals("", -6, digits.toScientific(9, 3));
        verifyDigitList(
                "43561000000",
                digits);
    }
}

void NumberFormat2Test::TestRounding() {
    DigitList digits;
    uprv_decContextSetRounding(&digits.fContext, DEC_ROUND_CEILING);
    {
        // Round at very large exponent
        digits.set(789.123);
        digits.roundAtExponent(100);
        verifyDigitList(
                "10000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000", // 100 0's after 1
                digits);
    }
    {
        // Round at very large exponent
        digits.set(789.123);
        digits.roundAtExponent(1);
        verifyDigitList(
                "790", // 100 0's after 1
                digits);
    }
    {
        // Round at positive exponent
        digits.set(789.123);
        digits.roundAtExponent(1);
        verifyDigitList("790", digits);
    }
    {
        // Round at zero exponent
        digits.set(788.123);
        digits.roundAtExponent(0);
        verifyDigitList("789", digits);
    }
    {
        // Round at negative exponent
        digits.set(789.123);
        digits.roundAtExponent(-2);
        verifyDigitList("789.13", digits);
    }
    {
        // Round to exponent of digits.
        digits.set(789.123);
        digits.roundAtExponent(-3);
        verifyDigitList("789.123", digits);
    }
    {
        // Round at large negative exponent
        digits.set(789.123);
        digits.roundAtExponent(-100);
        verifyDigitList("789.123", digits);
    }
    {
        // Round negative
        digits.set(-789.123);
        digits.roundAtExponent(-2);
        digits.setPositive(TRUE);
        verifyDigitList("789.12", digits);
    }
    {
        // Round to 1 significant digit
        digits.set(789.123);
        digits.roundAtExponent(INT32_MIN, 1);
        verifyDigitList("800", digits);
    }
    {
        // Round to 5 significant digit
        digits.set(789.123);
        digits.roundAtExponent(INT32_MIN, 5);
        verifyDigitList("789.13", digits);
    }
    {
        // Round to 6 significant digit
        digits.set(789.123);
        digits.roundAtExponent(INT32_MIN, 6);
        verifyDigitList("789.123", digits);
    }
    {
        // no-op
        digits.set(789.123);
        digits.roundAtExponent(INT32_MIN, INT32_MAX);
        verifyDigitList("789.123", digits);
    }
    {
        // Rounding at -1 produces fewer than 5 significant digits
        digits.set(789.123);
        digits.roundAtExponent(-1, 5);
        verifyDigitList("789.2", digits);
    }
    {
        // Rounding at -1 produces exactly 4 significant digits
        digits.set(789.123);
        digits.roundAtExponent(-1, 4);
        verifyDigitList("789.2", digits);
    }
    {
        // Rounding at -1 produces more than 3 significant digits
        digits.set(788.123);
        digits.roundAtExponent(-1, 3);
        verifyDigitList("789", digits);
    }
    {
        digits.set(123.456);
        digits.round(INT32_MAX);
        verifyDigitList("123.456", digits);
    }
    {
        digits.set(123.456);
        digits.round(1);
        verifyDigitList("200", digits);
    }
}
void NumberFormat2Test::TestBenchmark() {
/*
    UErrorCode status = U_ZERO_ERROR;
    DecimalFormatSymbols symbols("en", status);
    DigitFormatter formatter(symbols);
    SciFormatter sciformatter(symbols);
    ScientificPrecision precision;
    precision.fMantissa.fMax.setFracDigitCount(5);
    SciFormatterOptions options;
    ValueFormatter vf;
    vf.prepareScientificFormatting(
            sciformatter, formatter, precision, options);
    DigitAffixesAndPadding aap;
    aap.fNegativePrefix.append("-", UNUM_SIGN_FIELD);
    FieldPosition fpos(FieldPosition::DONT_CARE);
    clock_t start = clock();
    for (int32_t i = 0; i < 1000000; ++i) {
        UnicodeString appendTo;
        DigitList digits;
        digits.set(299792458);
        FieldPositionOnlyHandler handler(fpos);
        aap.format(
                digits,
                vf,
                handler,
                NULL,
                appendTo,
                status);
    }
    errln("Took %f", (double) (clock() - start) / CLOCKS_PER_SEC);
    UErrorCode status = U_ZERO_ERROR;
    DecimalFormatSymbols symbols("en", status);
    DigitFormatter formatter(symbols);
    DigitFormatterIntOptions options;
    FieldPosition fpos(FieldPosition::DONT_CARE);
//    FieldPosition fpos(3);
    FieldPositionOnlyHandler handler(fpos);
    clock_t start = clock();
    for (int32_t i = 0; i < 10000000; ++i) {
        UnicodeString appendTo;
        formatter.formatInt32(2345, options, 0, 0, handler, appendTo);
    }
    errln("Took %f", (double) (clock() - start) / CLOCKS_PER_SEC);
*/
}

void NumberFormat2Test::TestBenchmark2() {
/*
    UErrorCode status = U_ZERO_ERROR;
    UParseError perror;
    NumberFormat *nf = NumberFormat::createScientificInstance("en", status);
    nf->setMaximumFractionDigits(5);
    FieldPosition fpos(FieldPosition::DONT_CARE);
    clock_t start = clock();
    for (int32_t i = 0; i < 1000000; ++i) {
        UnicodeString appendTo;
        DigitList digits;
        digits.set(299792458);
        nf->format(digits, appendTo, fpos, status);
    }
    errln("Took %f", (double) (clock() - start) / CLOCKS_PER_SEC);
    UErrorCode status = U_ZERO_ERROR;
    UParseError perror;
    DecimalFormatSymbols symbols("en", status);
    DecimalFormatPatternParser parser;
    parser.useSymbols(symbols);
    UnicodeString pattern("\u00a4\u00a4% 0.00;\u00a4\u00a4 (-#)");
    pattern = pattern.unescape();
    DecimalFormatPattern out;
    parser.applyPatternWithoutExpandAffix(
            pattern, out, perror, status);
    assertSuccess("", status);
    errln(out.fPosPrefixPattern);
    errln(out.fNegPrefixPattern);
    errln(out.fNegSuffixPattern);
*/
}


void NumberFormat2Test::TestDigitFormatterDefaultCtor() {
    DigitFormatter formatter;
    DigitList digits;
    DigitInterval interval;
    DigitGrouping grouping;
    digits.set(246.801);
    verifyDigitFormatter(
            "246.801",
            formatter,
            digits,
            grouping,
            digits.getSmallestInterval(interval),
            FALSE,
            NULL);
    verifyDigitIntFormatter(
            "+023",
            formatter,
            23,
            3,
            TRUE,
            NULL);
}

void NumberFormat2Test::TestDigitIntFormatter() {
    UErrorCode status = U_ZERO_ERROR;
    DecimalFormatSymbols symbols("en", status);
    DigitFormatter formatter(symbols);
    {
        verifyDigitIntFormatter(
                "0",
                formatter,
                0,
                0,
                FALSE,
                NULL);
    }
    {
        verifyDigitIntFormatter(
                "+0",
                formatter,
                0,
                0,
                TRUE,
                NULL);
    }
    {
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {kIntField, 0, 1},
            {0, -1, 0}};
        verifyDigitIntFormatter(
                "0",
                formatter,
                0,
                1,
                FALSE,
                expectedAttributes);
    }
    {
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {kSignField, 0, 1},
            {kIntField, 1, 2},
            {0, -1, 0}};
        verifyDigitIntFormatter(
                "+0",
                formatter,
                0,
                1,
                TRUE,
                expectedAttributes);
    }
    {
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {kSignField, 0, 1},
            {kIntField, 1, 2},
            {0, -1, 0}};
        verifyDigitIntFormatter(
                "-2",
                formatter,
                -2,
                1,
                FALSE,
                expectedAttributes);
    }
    {
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {kSignField, 0, 1},
            {kIntField, 1, 3},
            {0, -1, 0}};
        verifyDigitIntFormatter(
                "-02",
                formatter,
                -2,
                2,
                FALSE,
                expectedAttributes);
    }
    {
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {kSignField, 0, 1},
            {kIntField, 1, 3},
            {0, -1, 0}};
        verifyDigitIntFormatter(
                "-02",
                formatter,
                -2,
                2,
                TRUE,
                expectedAttributes);
    }
    {
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {kSignField, 0, 1},
            {kIntField, 1, 11},
            {0, -1, 0}};
        verifyDigitIntFormatter(
                "-2147483648",
                formatter,
                INT32_MIN,
                1,
                FALSE,
                expectedAttributes);
    }
    {
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {kSignField, 0, 1},
            {kIntField, 1, 11},
            {0, -1, 0}};
        verifyDigitIntFormatter(
                "+2147483647",
                formatter,
                INT32_MAX,
                1,
                TRUE,
                expectedAttributes);
    }
    {
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {kIntField, 0, 9},
            {0, -1, 0}};
        verifyDigitIntFormatter(
                "007654321",
                formatter,
                7654321,
                9,
                FALSE,
                expectedAttributes);
    }
    // Test fast track boundaries
    {
        verifyDigitIntFormatter(
                "+23",
                formatter,
                23,
                0,
                TRUE,
                NULL);
        verifyDigitIntFormatter(
                "+000023",
                formatter,
                23,
                6,
                TRUE,
                NULL);
        verifyDigitIntFormatter(
                "00023",
                formatter,
                23,
                5,
                FALSE,
                NULL);
        verifyDigitIntFormatter(
                "4096",
                formatter,
                4096,
                0,
                FALSE,
                NULL);
    }
    // Test fast track
    {
        verifyDigitIntFormatter(
                "23",
                formatter,
                23,
                0,
                FALSE,
                NULL);
        verifyDigitIntFormatter(
                "23",
                formatter,
                23,
                1,
                FALSE,
                NULL);
        verifyDigitIntFormatter(
                "23",
                formatter,
                23,
                2,
                FALSE,
                NULL);
        verifyDigitIntFormatter(
                "023",
                formatter,
                23,
                3,
                FALSE,
                NULL);
        verifyDigitIntFormatter(
                "0023",
                formatter,
                23,
                4,
                FALSE,
                NULL);
    }
    // Test fast track digit count boundaries
    {
        verifyDigitIntFormatter(
                "9",
                formatter,
                9,
                0,
                FALSE,
                NULL);
        verifyDigitIntFormatter(
                "10",
                formatter,
                10,
                0,
                FALSE,
                NULL);
        verifyDigitIntFormatter(
                "99",
                formatter,
                99,
                0,
                FALSE,
                NULL);
        verifyDigitIntFormatter(
                "100",
                formatter,
                100,
                0,
                FALSE,
                NULL);
        verifyDigitIntFormatter(
                "999",
                formatter,
                999,
                0,
                FALSE,
                NULL);
        verifyDigitIntFormatter(
                "1000",
                formatter,
                1000,
                0,
                FALSE,
                NULL);
        verifyDigitIntFormatter(
                "4095",
                formatter,
                4095,
                0,
                FALSE,
                NULL);
    }
}

void NumberFormat2Test::TestDigitFormatter() {
    UErrorCode status = U_ZERO_ERROR;
    DecimalFormatSymbols symbols("en", status);
    DigitFormatter formatter(symbols);
    DigitList digits;
    DigitInterval interval;
    {
        DigitGrouping grouping;
        digits.set(8192);
        verifyDigitFormatter(
                "8192",
                formatter,
                digits,
                grouping,
                digits.getSmallestInterval(interval),
                FALSE,
                NULL);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_INTEGER_FIELD, 0, 4},
            {UNUM_DECIMAL_SEPARATOR_FIELD, 4, 5},
            {0, -1, 0}};
        verifyDigitFormatter(
                "8192.",
                formatter,
                digits,
                grouping,
                digits.getSmallestInterval(interval),
                TRUE,
                expectedAttributes);

        // Turn on grouping
        grouping.fGrouping = 3;
        verifyDigitFormatter(
                "8,192",
                formatter,
                digits,
                grouping,
                digits.getSmallestInterval(interval),
                FALSE,
                NULL);

        // turn on min grouping which will suppress grouping
        grouping.fMinGrouping = 2;
        verifyDigitFormatter(
                "8192",
                formatter,
                digits,
                grouping,
                digits.getSmallestInterval(interval),
                FALSE,
                NULL);

        // adding one more digit will enable grouping once again.
        digits.set(43560);
        verifyDigitFormatter(
                "43,560",
                formatter,
                digits,
                grouping,
                digits.getSmallestInterval(interval),
                FALSE,
                NULL);
    }
    {
        DigitGrouping grouping;
        digits.set(31415926.0078125);
        verifyDigitFormatter(
                "31415926.0078125",
                formatter,
                digits,
                grouping,
                digits.getSmallestInterval(interval),
                FALSE,
                NULL);

        // Turn on grouping with secondary.
        grouping.fGrouping = 2;
        grouping.fGrouping2 = 3;
        verifyDigitFormatter(
                "314,159,26.0078125",
                formatter,
                digits,
                grouping,
                digits.getSmallestInterval(interval),
                FALSE,
                NULL);

        // Pad with zeros by widening interval.
        interval.setIntDigitCount(9);
        interval.setFracDigitCount(10);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_GROUPING_SEPARATOR_FIELD, 1, 2},
            {UNUM_GROUPING_SEPARATOR_FIELD, 5, 6},
            {UNUM_GROUPING_SEPARATOR_FIELD, 9, 10},
            {UNUM_INTEGER_FIELD, 0, 12},
            {UNUM_DECIMAL_SEPARATOR_FIELD, 12, 13},
            {UNUM_FRACTION_FIELD, 13, 23},
            {0, -1, 0}};
        verifyDigitFormatter(
                "0,314,159,26.0078125000",
                formatter,
                digits,
                grouping,
                interval,
                FALSE,
                expectedAttributes);
    }
}

void NumberFormat2Test::TestSciFormatterDefaultCtor() {
    SciFormatter sciformatter;
    DigitFormatter formatter;
    DigitList mantissa;
    DigitInterval interval;
    SciFormatterOptions options;
    {
        mantissa.set(6.02);
        verifySciFormatter(
                "6.02E23",
                sciformatter,
                mantissa,
                23,
                formatter,
                mantissa.getSmallestInterval(interval),
                options,
                NULL);
    }
    {
        mantissa.set(6.62);
        verifySciFormatter(
                "6.62E-34",
                sciformatter,
                mantissa,
                -34,
                formatter,
                mantissa.getSmallestInterval(interval),
                options,
                NULL);
    }
}

void NumberFormat2Test::TestSciFormatter() {
    UErrorCode status = U_ZERO_ERROR;
    DecimalFormatSymbols symbols("en", status);
    SciFormatter sciformatter(symbols);
    DigitFormatter formatter(symbols);
    DigitList mantissa;
    DigitInterval interval;
    SciFormatterOptions options;
    options.fExponent.fMinDigits = 3;
    {
        options.fExponent.fAlwaysShowSign = TRUE;
        mantissa.set(1248);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_INTEGER_FIELD, 0, 4},
            {UNUM_EXPONENT_SYMBOL_FIELD, 4, 5},
            {UNUM_EXPONENT_SIGN_FIELD, 5, 6},
            {UNUM_EXPONENT_FIELD, 6, 9},
            {0, -1, 0}};
        verifySciFormatter(
                "1248E+023",
                sciformatter,
                mantissa,
                23,
                formatter,
                mantissa.getSmallestInterval(interval),
                options,
                expectedAttributes);
    }
    {
        options.fMantissa.fAlwaysShowDecimal = TRUE;
        options.fExponent.fAlwaysShowSign = FALSE;
        mantissa.set(1248);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_INTEGER_FIELD, 0, 4},
            {UNUM_DECIMAL_SEPARATOR_FIELD, 4, 5},
            {UNUM_EXPONENT_SYMBOL_FIELD, 5, 6},
            {UNUM_EXPONENT_FIELD, 6, 9},
            {0, -1, 0}};
        verifySciFormatter(
                "1248.E023",
                sciformatter,
                mantissa,
                23,
                formatter,
                mantissa.getSmallestInterval(interval),
                options,
                expectedAttributes);
    }
}

void NumberFormat2Test::TestValueFormatter() {
    UErrorCode status = U_ZERO_ERROR;
    DecimalFormatSymbols symbols("en", status);
    DigitFormatter formatter(symbols);
    DigitGrouping grouping;
    grouping.fGrouping = 3;
    FixedPrecision precision;
    precision.fMin.setIntDigitCount(4);
    precision.fMin.setFracDigitCount(2);
    precision.fMax.setIntDigitCount(6);
    precision.fMax.setFracDigitCount(4);
    DigitFormatterOptions options;
    ValueFormatter vf;
    vf.prepareFixedDecimalFormatting(
            formatter,
            grouping,
            precision,
            options);
    DigitList digits;
    {
        digits.set(3.49951);
        verifyValueFormatter(
                "0,003.4995",
                vf,
                digits,
                NULL);
    }
    {
        digits.set(3.499951);
        verifyValueFormatter(
                "0,003.50",
                vf,
                digits,
                NULL);
    }
    {
        digits.set(1234567.89008);
        verifyValueFormatter(
                "234,567.8901",
                vf,
                digits,
                NULL);
    }
    // significant digits too
    precision.fSignificant.setMin(3);
    precision.fSignificant.setMax(4);
    {
        digits.set(342.562);
        verifyValueFormatter(
                "0,342.60",
                vf,
                digits,
                NULL);
    }
    {
        digits.set(0.57);
        verifyValueFormatter(
                "0,000.570",
                vf,
                digits,
                NULL);
    }
}

void NumberFormat2Test::TestValueFormatterScientific() {
    UErrorCode status = U_ZERO_ERROR;
    DecimalFormatSymbols symbols("en", status);
    SciFormatter sciformatter(symbols);
    DigitFormatter formatter(symbols);
    ScientificPrecision precision;
    precision.fMantissa.fSignificant.setMax(3);
    SciFormatterOptions options;
    ValueFormatter vf;
    vf.prepareScientificFormatting(
            sciformatter,
            formatter,
            precision,
            options);
    DigitList digits;
    {
        digits.set(43560);
        verifyValueFormatter(
                "4.36E4",
                vf,
                digits,
                NULL);
    }
    {
        digits.set(43560);
        precision.fMantissa.fMax.setIntDigitCount(3);
        verifyValueFormatter(
                "43.6E3",
                vf,
                digits,
                NULL);
    }
    {
        digits.set(43560);
        precision.fMantissa.fMin.setIntDigitCount(3);
        verifyValueFormatter(
                "436E2",
                vf,
                digits,
                NULL);
    }
    {
        digits.set(43560);
        options.fExponent.fAlwaysShowSign = TRUE;
        options.fExponent.fMinDigits = 2;
        verifyValueFormatter(
                "436E+02",
                vf,
                digits,
                NULL);
    }

}

void NumberFormat2Test::TestDigitAffix() {
    DigitAffix affix;
    {
        affix.append("foo");
        affix.append("--", UNUM_SIGN_FIELD);
        affix.append("%", UNUM_PERCENT_FIELD);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_SIGN_FIELD, 3, 5},
            {UNUM_PERCENT_FIELD, 5, 6},
            {0, -1, 0}};
        verifyAffix("foo--%", affix, expectedAttributes);
    }
    {
        affix.remove();
        affix.append("USD", UNUM_CURRENCY_FIELD);
        affix.append(" ");
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_CURRENCY_FIELD, 0, 3},
            {0, -1, 0}};
        verifyAffix("USD ", affix, expectedAttributes);
    }
}

void NumberFormat2Test::TestPluralAffix() {
    UErrorCode status = U_ZERO_ERROR;
    PluralAffix part;
    part.setVariant("one", "Dollar", status);
    part.setVariant("few", "DollarFew", status);
    part.setVariant("other", "Dollars", status);
    PluralAffix dollar(part);
    PluralAffix percent(part);
    part.remove();
    part.setVariant("one", "Percent", status);
    part.setVariant("many", "PercentMany", status);
    part.setVariant("other", "Percents", status);
    percent = part;
    part.remove();
    part.setVariant("one", "foo", status);

    PluralAffix pa;
    assertEquals("", "", pa.getOtherVariant().toString());
    pa.append(dollar, UNUM_CURRENCY_FIELD, status);
    pa.append(" and ");
    pa.append(percent, UNUM_PERCENT_FIELD, status);
    pa.append("-", UNUM_SIGN_FIELD);

    {
        // other
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_CURRENCY_FIELD, 0, 7},
            {UNUM_PERCENT_FIELD, 12, 20},
            {UNUM_SIGN_FIELD, 20, 21},
            {0, -1, 0}};
        verifyAffix(
                "Dollars and Percents-",
                pa.getByVariant("other"),
                expectedAttributes);
    }
    {
        // two which is same as other
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_CURRENCY_FIELD, 0, 7},
            {UNUM_PERCENT_FIELD, 12, 20},
            {UNUM_SIGN_FIELD, 20, 21},
            {0, -1, 0}};
        verifyAffix(
                "Dollars and Percents-",
                pa.getByVariant("two"),
                expectedAttributes);
    }
    {
        // bad which is same as other
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_CURRENCY_FIELD, 0, 7},
            {UNUM_PERCENT_FIELD, 12, 20},
            {UNUM_SIGN_FIELD, 20, 21},
            {0, -1, 0}};
        verifyAffix(
                "Dollars and Percents-",
                pa.getByVariant("bad"),
                expectedAttributes);
    }
    {
        // one
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_CURRENCY_FIELD, 0, 6},
            {UNUM_PERCENT_FIELD, 11, 18},
            {UNUM_SIGN_FIELD, 18, 19},
            {0, -1, 0}};
        verifyAffix(
                "Dollar and Percent-",
                pa.getByVariant("one"),
                expectedAttributes);
    }
    {
        // few
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_CURRENCY_FIELD, 0, 9},
            {UNUM_PERCENT_FIELD, 14, 22},
            {UNUM_SIGN_FIELD, 22, 23},
            {0, -1, 0}};
        verifyAffix(
                "DollarFew and Percents-",
                pa.getByVariant("few"),
                expectedAttributes);
    }
    {
        // many
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_CURRENCY_FIELD, 0, 7},
            {UNUM_PERCENT_FIELD, 12, 23},
            {UNUM_SIGN_FIELD, 23, 24},
            {0, -1, 0}};
        verifyAffix(
                "Dollars and PercentMany-",
                pa.getByVariant("many"),
                expectedAttributes);
    }
    assertTrue("", pa.hasMultipleVariants());
    pa.remove();
    pa.append("$$$", UNUM_CURRENCY_FIELD);
    assertFalse("", pa.hasMultipleVariants());
    
}

void NumberFormat2Test::TestCurrencyAffixInfo() {
    CurrencyAffixInfo info;
    UnicodeString expectedSymbol("\u00a4");
    UnicodeString expectedSymbolIso("\u00a4\u00a4");
    UnicodeString expectedSymbols("\u00a4\u00a4\u00a4");
    assertEquals("", expectedSymbol.unescape(), info.fSymbol);
    assertEquals("", expectedSymbolIso.unescape(), info.fISO);
    assertEquals("", expectedSymbols.unescape(), info.fLong.getByVariant("one").toString());
    assertEquals("", expectedSymbols.unescape(), info.fLong.getByVariant("other").toString());
    assertEquals("", expectedSymbols.unescape(), info.fLong.getByVariant("two").toString());
    UErrorCode status = U_ZERO_ERROR;
    static UChar USD[] = {0x55, 0x53, 0x44, 0x0};
    LocalPointer<PluralRules> rules(PluralRules::forLocale("en", status));
    info.set("en", rules.getAlias(), USD, status);
    assertEquals("", "$", info.fSymbol);
    assertEquals("", "USD", info.fISO);
    assertEquals("", "US dollar", info.fLong.getByVariant("one").toString());
    assertEquals("", "US dollars", info.fLong.getByVariant("other").toString());
    assertEquals("", "US dollars", info.fLong.getByVariant("two").toString());
    info.set(NULL, NULL, NULL, status);
    assertEquals("", expectedSymbol.unescape(), info.fSymbol);
    assertEquals("", expectedSymbolIso.unescape(), info.fISO);
    assertEquals("", expectedSymbols.unescape(), info.fLong.getByVariant("one").toString());
    assertEquals("", expectedSymbols.unescape(), info.fLong.getByVariant("other").toString());
    assertEquals("", expectedSymbols.unescape(), info.fLong.getByVariant("two").toString());
}

void NumberFormat2Test::TestAffixPatternParser() {
    UErrorCode status = U_ZERO_ERROR;
    static UChar USD[] = {0x55, 0x53, 0x44};
    LocalPointer<PluralRules> rules(PluralRules::forLocale("en", status));
    DecimalFormatSymbols symbols("en", status);
    AffixPatternParser parser(symbols);
    parser.fCurrencyAffixInfo.set("en", rules.getAlias(), USD, status);
    PluralAffix affix;
    UnicodeString str("'--y'''dz'%'\u00a4\u00a4\u00a4\u00a4 y '\u00a4\u00a4\u00a4 or '\u00a4\u00a4 but '\u00a4");
    str = str.unescape();
    assertSuccess("", status);
    AffixPattern affixPattern;
    assertEquals(
            "",
            2,
            parser.parse(
                    AffixPattern::parseAffixString(str, affixPattern, status),
                    affix,
                    status));
    assertSuccess("", status);
    assertTrue("", affixPattern.usesCurrency());
    assertTrue("", affix.hasMultipleVariants());
    {
        // other
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_SIGN_FIELD, 0, 1},
            {UNUM_PERCENT_FIELD, 6, 7},
            {UNUM_CURRENCY_FIELD, 7, 17},
            {UNUM_CURRENCY_FIELD, 21, 31},
            {UNUM_CURRENCY_FIELD, 35, 38},
            {UNUM_CURRENCY_FIELD, 43, 44},
            {0, -1, 0}};
        verifyAffix(
                "--y'dz%US dollars\u00a4 y US dollars or USD but $",
                affix.getByVariant("other"),
                expectedAttributes);
    }
    {
        // one
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_SIGN_FIELD, 0, 1},
            {UNUM_PERCENT_FIELD, 6, 7},
            {UNUM_CURRENCY_FIELD, 7, 16},
            {UNUM_CURRENCY_FIELD, 20, 29},
            {UNUM_CURRENCY_FIELD, 33, 36},
            {UNUM_CURRENCY_FIELD, 41, 42},
            {0, -1, 0}};
        verifyAffix(
                "--y'dz%US dollar\u00a4 y US dollar or USD but $",
                affix.getByVariant("one"),
                expectedAttributes);
    }
    affix.remove();
    str = "%'-";
    affixPattern.remove();
    assertEquals(
            "",
            0,
            parser.parse(
                    AffixPattern::parseAffixString(str, affixPattern, status),
                    affix,
                    status));
    assertSuccess("", status);
    assertFalse("", affixPattern.usesCurrency());
    assertFalse("", affix.hasMultipleVariants());
    {
        // other
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_SIGN_FIELD, 1, 2},
            {0, -1, 0}};
        verifyAffix(
                "%-",
                affix.getByVariant("other"),
                expectedAttributes);
    }
    UnicodeString a4("\u00a4");
    AffixPattern scratchPattern;
    AffixPattern::parseAffixString(a4.unescape(), scratchPattern, status);
    assertFalse("", scratchPattern.usesCurrency());

    // Test really long string > 256 chars.
    str = "'%012345678901234567890123456789012345678901234567890123456789"
          "012345678901234567890123456789012345678901234567890123456789"
          "012345678901234567890123456789012345678901234567890123456789"
          "012345678901234567890123456789012345678901234567890123456789"
          "012345678901234567890123456789012345678901234567890123456789";
    affixPattern.remove();
    affix.remove();
    assertEquals(
            "",
            2,
            parser.parse(
                    AffixPattern::parseAffixString(str, affixPattern, status),
                    affix,
                    status));
    assertSuccess("", status);
    assertFalse("", affixPattern.usesCurrency());
    assertFalse("", affix.hasMultipleVariants());
    {
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_PERCENT_FIELD, 0, 1},
            {0, -1, 0}};
        verifyAffix(
                "%012345678901234567890123456789012345678901234567890123456789"
                "012345678901234567890123456789012345678901234567890123456789"
                "012345678901234567890123456789012345678901234567890123456789"
                "012345678901234567890123456789012345678901234567890123456789"
                "012345678901234567890123456789012345678901234567890123456789",
                affix.getOtherVariant(),
                expectedAttributes);
    }
  

}


void NumberFormat2Test::TestDigitAffixesAndPadding() {
    UErrorCode status = U_ZERO_ERROR;
    DecimalFormatSymbols symbols("en", status);
    DigitFormatter formatter(symbols);
    DigitGrouping grouping;
    grouping.fGrouping = 3;
    FixedPrecision precision;
    DigitFormatterOptions options;
    options.fAlwaysShowDecimal = TRUE;
    ValueFormatter vf;
    vf.prepareFixedDecimalFormatting(
            formatter,
            grouping,
            precision,
            options);
    DigitList digits;
    DigitAffixesAndPadding aap;
    aap.fPositivePrefix.append("(+", UNUM_SIGN_FIELD);
    aap.fPositiveSuffix.append("+)", UNUM_SIGN_FIELD);
    aap.fNegativePrefix.append("(-", UNUM_SIGN_FIELD);
    aap.fNegativeSuffix.append("-)", UNUM_SIGN_FIELD);
    aap.fWidth = 10;
    aap.fPadPosition = DigitAffixesAndPadding::kPadBeforePrefix;
    {
        digits.set(3);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_SIGN_FIELD, 4, 6},
            {UNUM_INTEGER_FIELD, 6, 7},
            {UNUM_DECIMAL_SEPARATOR_FIELD, 7, 8},
            {UNUM_SIGN_FIELD, 8, 10},
            {0, -1, 0}};
        verifyAffixesAndPadding(
                "****(+3.+)",
                aap,
                digits,
                vf,
                NULL,
                expectedAttributes);
    }
    aap.fPadPosition = DigitAffixesAndPadding::kPadAfterPrefix;
    {
        digits.set(3);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_SIGN_FIELD, 0, 2},
            {UNUM_INTEGER_FIELD, 6, 7},
            {UNUM_DECIMAL_SEPARATOR_FIELD, 7, 8},
            {UNUM_SIGN_FIELD, 8, 10},
            {0, -1, 0}};
        verifyAffixesAndPadding(
                "(+****3.+)",
                aap,
                digits,
                vf,
                NULL,
                expectedAttributes);
    }
    aap.fPadPosition = DigitAffixesAndPadding::kPadBeforeSuffix;
    {
        digits.set(3);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_SIGN_FIELD, 0, 2},
            {UNUM_INTEGER_FIELD, 2, 3},
            {UNUM_DECIMAL_SEPARATOR_FIELD, 3, 4},
            {UNUM_SIGN_FIELD, 8, 10},
            {0, -1, 0}};
        verifyAffixesAndPadding(
                "(+3.****+)",
                aap,
                digits,
                vf,
                NULL,
                expectedAttributes);
    }
    aap.fPadPosition = DigitAffixesAndPadding::kPadAfterSuffix;
    {
        digits.set(3);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_SIGN_FIELD, 0, 2},
            {UNUM_INTEGER_FIELD, 2, 3},
            {UNUM_DECIMAL_SEPARATOR_FIELD, 3, 4},
            {UNUM_SIGN_FIELD, 4, 6},
            {0, -1, 0}};
        verifyAffixesAndPadding(
                "(+3.+)****",
                aap,
                digits,
                vf,
                NULL,
                expectedAttributes);
    }
    aap.fPadPosition = DigitAffixesAndPadding::kPadAfterSuffix;
    {
        digits.set(-1234.5);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_SIGN_FIELD, 0, 2},
            {UNUM_GROUPING_SEPARATOR_FIELD, 3, 4},
            {UNUM_INTEGER_FIELD, 2, 7},
            {UNUM_DECIMAL_SEPARATOR_FIELD, 7, 8},
            {UNUM_FRACTION_FIELD, 8, 9},
            {UNUM_SIGN_FIELD, 9, 11},
            {0, -1, 0}};
        verifyAffixesAndPadding(
                "(-1,234.5-)",
                aap,
                digits,
                vf,
                NULL,
                expectedAttributes);
    }
    assertFalse("", aap.needsPluralRules());

    aap.fWidth = 0;
    aap.fPositivePrefix.remove();
    aap.fPositiveSuffix.remove();
    aap.fNegativePrefix.remove();
    aap.fNegativeSuffix.remove();
    
    // Set up for plural currencies.
    aap.fNegativePrefix.append("-", UNUM_SIGN_FIELD);
    {
        PluralAffix part;
        part.setVariant("one", " Dollar", status);
        part.setVariant("other", " Dollars", status);
        aap.fPositiveSuffix.append(part, UNUM_CURRENCY_FIELD, status);
    }
    aap.fNegativeSuffix = aap.fPositiveSuffix;
    
    LocalPointer<PluralRules> rules(PluralRules::forLocale("en", status));

    // Now test plurals
    assertTrue("", aap.needsPluralRules());
    {
        digits.set(1);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_INTEGER_FIELD, 0, 1},
            {UNUM_DECIMAL_SEPARATOR_FIELD, 1, 2},
            {UNUM_CURRENCY_FIELD, 2, 9},
            {0, -1, 0}};
        verifyAffixesAndPadding(
                "1. Dollar",
                aap,
                digits,
                vf,
                rules.getAlias(),
                expectedAttributes);
    }
    {
        digits.set(-1);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_SIGN_FIELD, 0, 1},
            {UNUM_INTEGER_FIELD, 1, 2},
            {UNUM_DECIMAL_SEPARATOR_FIELD, 2, 3},
            {UNUM_CURRENCY_FIELD, 3, 10},
            {0, -1, 0}};
        verifyAffixesAndPadding(
                "-1. Dollar",
                aap,
                digits,
                vf,
                rules.getAlias(),
                expectedAttributes);
    }
    precision.fMin.setFracDigitCount(2);
    {
        digits.set(1);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_INTEGER_FIELD, 0, 1},
            {UNUM_DECIMAL_SEPARATOR_FIELD, 1, 2},
            {UNUM_FRACTION_FIELD, 2, 4},
            {UNUM_CURRENCY_FIELD, 4, 12},
            {0, -1, 0}};
        verifyAffixesAndPadding(
                "1.00 Dollars",
                aap,
                digits,
                vf,
                rules.getAlias(),
                expectedAttributes);
    }
}

void NumberFormat2Test::TestPluralsAndRounding() {
    UErrorCode status = U_ZERO_ERROR;
    DecimalFormatSymbols symbols("en", status);
    DigitFormatter formatter(symbols);
    DigitGrouping grouping;
    FixedPrecision precision;
    precision.fSignificant.setMax(3);
    DigitFormatterOptions options;
    ValueFormatter vf;
    vf.prepareFixedDecimalFormatting(
            formatter,
            grouping,
            precision,
            options);
    DigitList digits;
    DigitAffixesAndPadding aap;
    // Set up for plural currencies.
    aap.fNegativePrefix.append("-", UNUM_SIGN_FIELD);
    {
        PluralAffix part;
        part.setVariant("one", " Dollar", status);
        part.setVariant("other", " Dollars", status);
        aap.fPositiveSuffix.append(part, UNUM_CURRENCY_FIELD, status);
    }
    aap.fNegativeSuffix = aap.fPositiveSuffix;
    aap.fWidth = 14;
    LocalPointer<PluralRules> rules(PluralRules::forLocale("en", status));
    {
        digits.set(0.999);
        verifyAffixesAndPadding(
                "*0.999 Dollars",
                aap,
                digits,
                vf,
                rules.getAlias(),
                NULL);
    }
    {
        digits.set(0.9996);
        verifyAffixesAndPadding(
                "******1 Dollar",
                aap,
                digits,
                vf,
                rules.getAlias(),
                NULL);
    }
    {
        digits.set(1.004);
        verifyAffixesAndPadding(
                "******1 Dollar",
                aap,
                digits,
                vf,
                rules.getAlias(),
                NULL);
    }
    precision.fSignificant.setMin(2);
    {
        digits.set(0.9996);
        verifyAffixesAndPadding(
                "***1.0 Dollars",
                aap,
                digits,
                vf,
                rules.getAlias(),
                NULL);
    }
    {
        digits.set(1.004);
        verifyAffixesAndPadding(
                "***1.0 Dollars",
                aap,
                digits,
                vf,
                rules.getAlias(),
                NULL);
    }
    precision.fSignificant.setMin(0);
    {
        digits.set(-79.214);
        verifyAffixesAndPadding(
                "*-79.2 Dollars",
                aap,
                digits,
                vf,
                rules.getAlias(),
                NULL);
    }
    // No more sig digits just max fractions
    precision.fSignificant.setMax(0); 
    precision.fMax.setFracDigitCount(4);
    {
        digits.set(79.213562);
        verifyAffixesAndPadding(
                "79.2136 Dollars",
                aap,
                digits,
                vf,
                rules.getAlias(),
                NULL);
    }

}


void NumberFormat2Test::TestPluralsAndRoundingScientific() {
    UErrorCode status = U_ZERO_ERROR;
    DecimalFormatSymbols symbols("en", status);
    DigitFormatter formatter(symbols);
    SciFormatter sciformatter(symbols);
    ScientificPrecision precision;
    precision.fMantissa.fSignificant.setMax(4);
    SciFormatterOptions options;
    ValueFormatter vf;
    vf.prepareScientificFormatting(
            sciformatter,
            formatter,
            precision,
            options);
    DigitList digits;
    DigitAffixesAndPadding aap;
    aap.fNegativePrefix.append("-", UNUM_SIGN_FIELD);
    {
        PluralAffix part;
        part.setVariant("one", " Meter", status);
        part.setVariant("other", " Meters", status);
        aap.fPositiveSuffix.append(part, UNUM_FIELD_COUNT, status);
    }
    aap.fNegativeSuffix = aap.fPositiveSuffix;
    LocalPointer<PluralRules> rules(PluralRules::forLocale("en", status));
    {
        digits.set(0.99996);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_INTEGER_FIELD, 0, 1},
            {UNUM_EXPONENT_SYMBOL_FIELD, 1, 2},
            {UNUM_EXPONENT_FIELD, 2, 3},
            {0, -1, 0}};
        verifyAffixesAndPadding(
                "1E0 Meters",
                aap,
                digits,
                vf,
                rules.getAlias(),
                expectedAttributes);
    }
    options.fMantissa.fAlwaysShowDecimal = TRUE;
    {
        digits.set(0.99996);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_INTEGER_FIELD, 0, 1},
            {UNUM_DECIMAL_SEPARATOR_FIELD, 1, 2},
            {UNUM_EXPONENT_SYMBOL_FIELD, 2, 3},
            {UNUM_EXPONENT_FIELD, 3, 4},
            {0, -1, 0}};
        verifyAffixesAndPadding(
                "1.E0 Meters",
                aap,
                digits,
                vf,
                rules.getAlias(),
                expectedAttributes);
    }
    {
        digits.set(-299792458);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_SIGN_FIELD, 0, 1},
            {UNUM_INTEGER_FIELD, 1, 2},
            {UNUM_DECIMAL_SEPARATOR_FIELD, 2, 3},
            {UNUM_FRACTION_FIELD, 3, 6},
            {UNUM_EXPONENT_SYMBOL_FIELD, 6, 7},
            {UNUM_EXPONENT_FIELD, 7, 8},
            {0, -1, 0}};
        verifyAffixesAndPadding(
                "-2.998E8 Meters",
                aap,
                digits,
                vf,
                rules.getAlias(),
                expectedAttributes);
    }
    precision.fMantissa.fSignificant.setMin(4);
    options.fExponent.fAlwaysShowSign = TRUE;
    options.fExponent.fMinDigits = 3;
    {
        digits.set(3);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_INTEGER_FIELD, 0, 1},
            {UNUM_DECIMAL_SEPARATOR_FIELD, 1, 2},
            {UNUM_FRACTION_FIELD, 2, 5},
            {UNUM_EXPONENT_SYMBOL_FIELD, 5, 6},
            {UNUM_EXPONENT_SIGN_FIELD, 6, 7},
            {UNUM_EXPONENT_FIELD, 7, 10},
            {0, -1, 0}};
        verifyAffixesAndPadding(
                "3.000E+000 Meters",
                aap,
                digits,
                vf,
                rules.getAlias(),
                expectedAttributes);
    }
    precision.fMantissa.fMax.setIntDigitCount(3);
    {
        digits.set(0.00025001);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_INTEGER_FIELD, 0, 3},
            {UNUM_DECIMAL_SEPARATOR_FIELD, 3, 4},
            {UNUM_FRACTION_FIELD, 4, 5},
            {UNUM_EXPONENT_SYMBOL_FIELD, 5, 6},
            {UNUM_EXPONENT_SIGN_FIELD, 6, 7},
            {UNUM_EXPONENT_FIELD, 7, 10},
            {0, -1, 0}};
        verifyAffixesAndPadding(
                "250.0E-006 Meters",
                aap,
                digits,
                vf,
                rules.getAlias(),
                expectedAttributes);
    }
    {
        digits.set(0.0000025001);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_INTEGER_FIELD, 0, 1},
            {UNUM_DECIMAL_SEPARATOR_FIELD, 1, 2},
            {UNUM_FRACTION_FIELD, 2, 5},
            {UNUM_EXPONENT_SYMBOL_FIELD, 5, 6},
            {UNUM_EXPONENT_SIGN_FIELD, 6, 7},
            {UNUM_EXPONENT_FIELD, 7, 10},
            {0, -1, 0}};
        verifyAffixesAndPadding(
                "2.500E-006 Meters",
                aap,
                digits,
                vf,
                rules.getAlias(),
                expectedAttributes);
    }
    precision.fMantissa.fMax.setFracDigitCount(1);
    {
        digits.set(0.0000025499);
        NumberFormat2Test_Attributes expectedAttributes[] = {
            {UNUM_INTEGER_FIELD, 0, 1},
            {UNUM_DECIMAL_SEPARATOR_FIELD, 1, 2},
            {UNUM_FRACTION_FIELD, 2, 3},
            {UNUM_EXPONENT_SYMBOL_FIELD, 3, 4},
            {UNUM_EXPONENT_SIGN_FIELD, 4, 5},
            {UNUM_EXPONENT_FIELD, 5, 8},
            {0, -1, 0}};
        verifyAffixesAndPadding(
                "2.5E-006 Meters",
                aap,
                digits,
                vf,
                rules.getAlias(),
                expectedAttributes);
    }
    precision.fMantissa.fMax.setIntDigitCount(1);
    precision.fMantissa.fMax.setFracDigitCount(2);
    {
        digits.set(299792458);
        verifyAffixesAndPadding(
                "3.00E+008 Meters",
                aap,
                digits,
                vf,
                rules.getAlias(),
                NULL);
    }
    // clear significant digits
    precision.fMantissa.fSignificant.setMin(0);
    precision.fMantissa.fSignificant.setMax(0);

    // set int and fraction digits
    precision.fMantissa.fMin.setFracDigitCount(2);
    precision.fMantissa.fMax.setFracDigitCount(4);
    precision.fMantissa.fMin.setIntDigitCount(2);
    precision.fMantissa.fMax.setIntDigitCount(3);
    {
        digits.set(-0.0000025300001);
        verifyAffixesAndPadding(
                "-253.00E-008 Meters",
                aap,
                digits,
                vf,
                rules.getAlias(),
                NULL);
    }
    {
        digits.set(-0.0000025300006);
        verifyAffixesAndPadding(
                "-253.0001E-008 Meters",
                aap,
                digits,
                vf,
                rules.getAlias(),
                NULL);
    }
    {
        digits.set(-0.000025300006);
        verifyAffixesAndPadding(
                "-25.30E-006 Meters",
                aap,
                digits,
                vf,
                rules.getAlias(),
                NULL);
    }
}


void NumberFormat2Test::TestRoundingIncrement() {
    UErrorCode status = U_ZERO_ERROR;
    DecimalFormatSymbols symbols("en", status);
    DigitFormatter formatter(symbols);
    SciFormatter sciformatter(symbols);
    ScientificPrecision precision;
    SciFormatterOptions options;
    precision.fMantissa.fRoundingIncrement.set(0.25);
    precision.fMantissa.fSignificant.setMax(4);
    DigitGrouping grouping;
    ValueFormatter vf;

    // fixed
    vf.prepareFixedDecimalFormatting(
            formatter,
            grouping,
            precision.fMantissa,
            options.fMantissa);
    DigitList digits;
    DigitAffixesAndPadding aap;
    aap.fNegativePrefix.append("-", UNUM_SIGN_FIELD);
    {
        digits.set(3.7);
        verifyAffixesAndPadding(
                "3.75",
                aap,
                digits,
                vf,
                NULL, NULL);
    }
    {
        digits.set(-7.4);
        verifyAffixesAndPadding(
                "-7.5",
                aap,
                digits,
                vf,
                NULL, NULL);
    }
    {
        digits.set(99.8);
        verifyAffixesAndPadding(
                "99.75",
                aap,
                digits,
                vf,
                NULL, NULL);
    }
    precision.fMantissa.fMin.setFracDigitCount(2);
    {
        digits.set(99.1);
        verifyAffixesAndPadding(
                "99.00",
                aap,
                digits,
                vf,
                NULL, NULL);
    }
    {
        digits.set(-639.65);
        verifyAffixesAndPadding(
                "-639.80",
                aap,
                digits,
                vf,
                NULL, NULL);
    }

    precision.fMantissa.fMin.setIntDigitCount(2);
    // Scientific notation
    vf.prepareScientificFormatting(
            sciformatter,
            formatter,
            precision,
            options);
    {
        digits.set(-6396.5);
        verifyAffixesAndPadding(
                "-64.00E2",
                aap,
                digits,
                vf,
                NULL, NULL);
    }
    {
        digits.set(-0.00092374);
        verifyAffixesAndPadding(
                "-92.25E-5",
                aap,
                digits,
                vf,
                NULL, NULL);
    }
    precision.fMantissa.fMax.setIntDigitCount(3);
    {
        digits.set(-0.00092374);
        verifyAffixesAndPadding(
                "-923.80E-6",
                aap,
                digits,
                vf,
                NULL, NULL);
    }
}


void NumberFormat2Test::TestDigitListToFixedDecimal() {
    DigitList digits;
    DigitInterval interval;
    digits.set(-9217.875);
    {
        interval.setIntDigitCount(2);
        interval.setFracDigitCount(1);
        FixedDecimal result(digits, interval);
        verifyFixedDecimal(result, 178, 10, TRUE, 1, 8);
    }
    {
        interval.setIntDigitCount(6);
        interval.setFracDigitCount(7);
        FixedDecimal result(digits, interval);
        verifyFixedDecimal(result, 9217875, 1000, TRUE, 7, 8750000);
    }
    {
        digits.set(1234.56);
        interval.setIntDigitCount(6);
        interval.setFracDigitCount(25);
        FixedDecimal result(digits, interval);
        verifyFixedDecimal(result, 123456, 100, FALSE, 18, 560000000000000000LL);
    }
}

void NumberFormat2Test::TestDataDriven() {
    NumberFormat2TestDataDriven dd(this);
//    dd.run("dumb.txt", FALSE);
    dd.run("numberformattestspecification.txt", TRUE);
}


void NumberFormat2Test::verifyFixedDecimal(
        const FixedDecimal &result,
        int64_t numerator,
        int64_t denominator,
        UBool bNegative,
        int32_t v,
        int64_t f) {
    assertEquals("", numerator, (int64_t) (result.source * (double) denominator + 0.5));
    assertEquals("", v, result.visibleDecimalDigitCount);
    assertEquals("", f, result.decimalDigits);
    assertEquals("", bNegative, result.isNegative);
}

void NumberFormat2Test::verifyAffixesAndPadding(
        const UnicodeString &expected,
        const DigitAffixesAndPadding &aaf,
        DigitList &digits,
        const ValueFormatter &vf,
        const PluralRules *optPluralRules,
        const NumberFormat2Test_Attributes *expectedAttributes) {
    UnicodeString appendTo;
    NumberFormat2Test_FieldPositionHandler handler;
    UErrorCode status = U_ZERO_ERROR;
    assertEquals(
            "",
            expected,
            aaf.format(
                    digits,
                    vf,
                    handler,
                    optPluralRules,
                    appendTo,
                    status));
    assertSuccess("", status);
    if (expectedAttributes != NULL) {
        verifyAttributes(expectedAttributes, handler.attributes);
    }
}

void NumberFormat2Test::verifyAffix(
        const UnicodeString &expected,
        const DigitAffix &affix,
        const NumberFormat2Test_Attributes *expectedAttributes) {
    UnicodeString appendTo;
    NumberFormat2Test_FieldPositionHandler handler;
    assertEquals(
            "",
            expected.unescape(),
            affix.format(handler, appendTo));
    if (expectedAttributes != NULL) {
        verifyAttributes(expectedAttributes, handler.attributes);
    }
}

void NumberFormat2Test::verifyValueFormatter(
        const UnicodeString &expected,
        const ValueFormatter &formatter,
        DigitList &digits,
        const NumberFormat2Test_Attributes *expectedAttributes) {
    UErrorCode status = U_ZERO_ERROR;
    formatter.round(digits, status);
    assertSuccess("", status);
    assertEquals(
            "",
            expected.countChar32(),
            formatter.countChar32(digits));
    UnicodeString appendTo;
    NumberFormat2Test_FieldPositionHandler handler;
    assertEquals(
            "",
            expected,
            formatter.format(
                    digits,
                    handler,
                    appendTo));
    if (expectedAttributes != NULL) {
        verifyAttributes(expectedAttributes, handler.attributes);
    }
}

// Right now only works for positive values.
void NumberFormat2Test::verifyDigitList(
        const UnicodeString &expected,
        const DigitList &digits) {
    DigitFormatter formatter;
    DigitInterval interval;
    DigitGrouping grouping;
    verifyDigitFormatter(
            expected,
            formatter,
            digits,
            grouping,
            digits.getSmallestInterval(interval),
            FALSE,
            NULL);
}

void NumberFormat2Test::verifyDigitIntFormatter(
        const UnicodeString &expected,
        const DigitFormatter &formatter,
        int32_t value,
        int32_t minDigits,
        UBool alwaysShowSign,
        const NumberFormat2Test_Attributes *expectedAttributes) {
    DigitFormatterIntOptions options;
    options.fMinDigits = minDigits;
    options.fAlwaysShowSign = alwaysShowSign;
    assertEquals(
            "",
            expected.countChar32(),
            formatter.countChar32ForInt(value, options));
    UnicodeString appendTo;
    NumberFormat2Test_FieldPositionHandler handler(expectedAttributes != NULL);
    assertEquals(
            "",
            expected,
            formatter.formatInt32(
                    value,
                    options,
                    kSignField,
                    kIntField,
                    handler,
                    appendTo));
    if (expectedAttributes != NULL) {
        verifyAttributes(expectedAttributes, handler.attributes);
    }
}

void NumberFormat2Test::verifySciFormatter(
        const UnicodeString &expected,
        const SciFormatter &sciformatter,
        const DigitList &mantissa,
        int32_t exponent,
        const DigitFormatter &formatter,
        const DigitInterval &interval,
        const SciFormatterOptions &options,
        const NumberFormat2Test_Attributes *expectedAttributes) {
    assertEquals(
            "",
            expected.countChar32(),
            sciformatter.countChar32(
                    exponent,
                    formatter,
                    interval,
                    options));
    UnicodeString appendTo;
    NumberFormat2Test_FieldPositionHandler handler;
    assertEquals(
            "",
            expected,
            sciformatter.format(
                    mantissa,
                    exponent,
                    formatter,
                    interval,
                    options,
                    handler,
                    appendTo));
    if (expectedAttributes != NULL) {
        verifyAttributes(expectedAttributes, handler.attributes);
    }
}

void NumberFormat2Test::verifyDigitFormatter(
        const UnicodeString &expected,
        const DigitFormatter &formatter,
        const DigitList &digits,
        const DigitGrouping &grouping,
        const DigitInterval &interval,
        UBool alwaysShowDecimal,
        const NumberFormat2Test_Attributes *expectedAttributes) {
    DigitFormatterOptions options;
    options.fAlwaysShowDecimal = alwaysShowDecimal;
    assertEquals(
            "",
            expected.countChar32(),
            formatter.countChar32(grouping, interval, options));
    UnicodeString appendTo;
    NumberFormat2Test_FieldPositionHandler handler;
    assertEquals(
            "",
            expected,
            formatter.format(
                    digits,
                    grouping,
                    interval,
                    options,
                    handler,
                    appendTo));
    if (expectedAttributes != NULL) {
        verifyAttributes(expectedAttributes, handler.attributes);
    }
}

void NumberFormat2Test::verifyAttributes(
        const NumberFormat2Test_Attributes *expected,
        const NumberFormat2Test_Attributes *actual) {
    int32_t idx = 0;
    while (expected[idx].spos != -1 && actual[idx].spos != -1) {
        assertEquals("id", expected[idx].id, actual[idx].id);
        assertEquals("spos", expected[idx].spos, actual[idx].spos);
        assertEquals("epos", expected[idx].epos, actual[idx].epos);
        ++idx;
    }
    assertEquals(
            "expected and actual not same length",
            expected[idx].spos,
            actual[idx].spos);
}

extern IntlTest *createNumberFormat2Test() {
    return new NumberFormat2Test();
}

#endif /* !UCONFIG_NO_FORMATTING */