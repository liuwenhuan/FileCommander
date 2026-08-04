#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>
#include <QTextCodec>

#include "ByteSearch.h"

// Pure QByteArray logic -- no widget is constructed here. The suite's
// QApplication is incidental.

namespace {

const QByteArray kUtf8 = QByteArrayLiteral("UTF-8");
const QByteArray kUtf16 = QByteArrayLiteral("UTF-16");

ByteSearch::Needle text(const QString &s, const QByteArray &codec = kUtf8,
                        ByteSearch::CaseFolding folding = ByteSearch::CaseFolding::Exact) {
    return ByteSearch::compile(s, ByteSearch::Mode::Text, codec, folding);
}

ByteSearch::Needle hex(const QString &s) {
    return ByteSearch::compile(s, ByteSearch::Mode::Hex, kUtf8, ByteSearch::CaseFolding::Exact);
}

} // namespace

TEST(ByteSearchTest, FindsAMatchThatStartsAtOffsetZero) {
    // The off-by-one every "search from the cursor" implementation gets wrong
    // first: a match sitting at the very start of the buffer.
    const QByteArray hay = QByteArrayLiteral("MZ header");
    const ByteSearch::Needle needle = text(QStringLiteral("MZ"));
    ASSERT_TRUE(needle.valid);
    EXPECT_EQ(ByteSearch::findForward(hay, needle, 0, false), 0);
    EXPECT_EQ(ByteSearch::countMatches(hay, needle), 1);
    EXPECT_EQ(ByteSearch::ordinalAt(hay, needle, 0), 1);
}

TEST(ByteSearchTest, ForwardSearchSkipsMatchesBeforeTheStartOffset) {
    const QByteArray hay = QByteArrayLiteral("ab-ab-ab");
    const ByteSearch::Needle needle = text(QStringLiteral("ab"));
    EXPECT_EQ(ByteSearch::findForward(hay, needle, 0, false), 0);
    EXPECT_EQ(ByteSearch::findForward(hay, needle, 1, false), 3);
    EXPECT_EQ(ByteSearch::findForward(hay, needle, 4, false), 6);
    EXPECT_EQ(ByteSearch::ordinalAt(hay, needle, 6), 3);
    EXPECT_EQ(ByteSearch::countMatches(hay, needle), 3);
}

TEST(ByteSearchTest, ForwardWrapsOnlyWhenAskedTo) {
    const QByteArray hay = QByteArrayLiteral("ab-ab");
    const ByteSearch::Needle needle = text(QStringLiteral("ab"));
    EXPECT_EQ(ByteSearch::findForward(hay, needle, 4, false), ByteSearch::kNotFound);
    EXPECT_EQ(ByteSearch::findForward(hay, needle, 4, true), 0);
}

TEST(ByteSearchTest, BackwardStepsOffTheCurrentMatchAndWraps) {
    const QByteArray hay = QByteArrayLiteral("ab-ab-ab");
    const ByteSearch::Needle needle = text(QStringLiteral("ab"));
    // `before` is the current match, so the same offset is never returned twice.
    EXPECT_EQ(ByteSearch::findBackward(hay, needle, 6, false), 3);
    EXPECT_EQ(ByteSearch::findBackward(hay, needle, 3, false), 0);
    EXPECT_EQ(ByteSearch::findBackward(hay, needle, 0, false), ByteSearch::kNotFound);
    EXPECT_EQ(ByteSearch::findBackward(hay, needle, 0, true), 6);
}

TEST(ByteSearchTest, ReportsNotFoundForANeedleThatIsNotThere) {
    const QByteArray hay = QByteArrayLiteral("the quick brown fox");
    const ByteSearch::Needle needle = text(QStringLiteral("zebra"));
    ASSERT_TRUE(needle.valid);
    EXPECT_EQ(ByteSearch::findForward(hay, needle, 0, true), ByteSearch::kNotFound);
    EXPECT_EQ(ByteSearch::findBackward(hay, needle, hay.size(), true), ByteSearch::kNotFound);
    EXPECT_EQ(ByteSearch::countMatches(hay, needle), 0);
}

TEST(ByteSearchTest, ANeedleLongerThanTheHaystackIsNotFoundRatherThanACrash) {
    const QByteArray hay = QByteArrayLiteral("ab");
    const ByteSearch::Needle needle = text(QStringLiteral("abcdef"));
    EXPECT_EQ(ByteSearch::findForward(hay, needle, 0, true), ByteSearch::kNotFound);
    EXPECT_EQ(ByteSearch::findBackward(hay, needle, hay.size(), true), ByteSearch::kNotFound);
    EXPECT_EQ(ByteSearch::ordinalAt(hay, needle, 0), 0);
}

TEST(ByteSearchTest, CountsOverlappingOccurrences) {
    // "aa" occurs at 0 and 1 in "aaa"; both are positions the user can be taken
    // to, so both are counted.
    const QByteArray hay = QByteArrayLiteral("aaa");
    const ByteSearch::Needle needle = text(QStringLiteral("aa"));
    EXPECT_EQ(ByteSearch::countMatches(hay, needle), 2);
    EXPECT_EQ(ByteSearch::findForward(hay, needle, 1, false), 1);
}

TEST(ByteSearchTest, ParsesTheThreeCommonHexSpellings) {
    const QByteArray expected = QByteArray::fromHex("4d5a");
    for (const QString &spelling : {QStringLiteral("4D 5A"), QStringLiteral("4d5a"),
                                    QStringLiteral("0x4D,0x5A"), QStringLiteral("\\x4d\\x5a"),
                                    QStringLiteral("4D-5A")}) {
        const ByteSearch::Needle needle = hex(spelling);
        EXPECT_TRUE(needle.valid) << spelling.toStdString() << ": "
                                  << needle.error.toStdString();
        EXPECT_EQ(needle.bytes, expected) << spelling.toStdString();
    }
}

TEST(ByteSearchTest, HexNeedleFindsBytesNoTextEncodingWouldProduce) {
    const QByteArray hay = QByteArray::fromHex("00004d5a9000");
    const ByteSearch::Needle needle = hex(QStringLiteral("4D 5A 90"));
    ASSERT_TRUE(needle.valid);
    EXPECT_EQ(ByteSearch::findForward(hay, needle, 0, false), 2);
}

TEST(ByteSearchTest, RejectsAnOddNumberOfNibblesWithAReason) {
    const ByteSearch::Needle unseparated = hex(QStringLiteral("4D5"));
    EXPECT_FALSE(unseparated.valid);
    EXPECT_FALSE(unseparated.error.isEmpty());
    EXPECT_TRUE(unseparated.bytes.isEmpty());

    // Ambiguity inside a group is just as unusable as an odd total: "4D 5 A"
    // has an even number of nibbles overall and still cannot be read.
    const ByteSearch::Needle grouped = hex(QStringLiteral("4D 5 A"));
    EXPECT_FALSE(grouped.valid);
    EXPECT_TRUE(grouped.error.contains(QLatin1Char('5')));
}

TEST(ByteSearchTest, RejectsNonHexCharactersWithAReason) {
    const ByteSearch::Needle needle = hex(QStringLiteral("4D ZZ"));
    EXPECT_FALSE(needle.valid);
    EXPECT_TRUE(needle.error.contains(QLatin1Char('Z'))) << needle.error.toStdString();
}

TEST(ByteSearchTest, AnEmptyQueryIsInvalidButNotAnError) {
    const ByteSearch::Needle empty = text(QString());
    EXPECT_FALSE(empty.valid);
    EXPECT_TRUE(empty.error.isEmpty()); // nothing to complain about yet
}

TEST(ByteSearchTest, TheSameNonAsciiNeedleFindsDifferentBytesPerEncoding) {
    // One buffer holding the same character twice, written two ways. This is
    // the whole reason ByteSearch exists rather than QByteArray::indexOf: the
    // needle's encoding decides which of the two is found, and getting it wrong
    // finds nothing at all.
    const QByteArray utf8Zhong = QByteArray::fromHex("e4b8ad");     // 中, UTF-8
    const QByteArray utf16Zhong = QByteArray::fromHex("2d4e");      // 中, UTF-16LE
    const QByteArray hay = QByteArrayLiteral("[") + utf8Zhong + QByteArrayLiteral("][") +
                           utf16Zhong + QByteArrayLiteral("]");

    const ByteSearch::Needle asUtf8 = text(QStringLiteral("中"), kUtf8);
    const ByteSearch::Needle asUtf16 = text(QStringLiteral("中"), kUtf16);
    ASSERT_TRUE(asUtf8.valid);
    ASSERT_TRUE(asUtf16.valid);
    EXPECT_EQ(asUtf8.bytes, utf8Zhong);
    // No byte-order mark: "UTF-16" writes one unless the encoder is asked not
    // to, and a needle starting FF FE matches nothing past a file's first byte.
    EXPECT_EQ(asUtf16.bytes, utf16Zhong);

    EXPECT_EQ(ByteSearch::findForward(hay, asUtf8, 0, true), 1);
    EXPECT_EQ(ByteSearch::findForward(hay, asUtf16, 0, true), 6);
    EXPECT_EQ(ByteSearch::countMatches(hay, asUtf8), 1);
    EXPECT_EQ(ByteSearch::countMatches(hay, asUtf16), 1);
}

TEST(ByteSearchTest, RefusesANeedleTheEncodingCannotWrite) {
    // Latin-1 substitutes '?' for 中. Searching for the substitute would answer
    // a question nobody asked, so it is refused instead.
    const ByteSearch::Needle needle = text(QStringLiteral("中"), QByteArrayLiteral("ISO-8859-1"));
    EXPECT_FALSE(needle.valid);
    EXPECT_FALSE(needle.error.isEmpty());
}

TEST(ByteSearchTest, CaseInsensitiveFoldsAsciiLettersOnly) {
    const QByteArray hay = QByteArrayLiteral("Mixed CASE case");
    const ByteSearch::Needle exact = text(QStringLiteral("case"));
    const ByteSearch::Needle folded =
        text(QStringLiteral("case"), kUtf8, ByteSearch::CaseFolding::AsciiInsensitive);
    ASSERT_EQ(folded.folding, ByteSearch::CaseFolding::AsciiInsensitive);
    EXPECT_TRUE(folded.note.isEmpty());

    EXPECT_EQ(ByteSearch::countMatches(hay, exact), 1);
    EXPECT_EQ(ByteSearch::countMatches(hay, folded), 2);
    EXPECT_EQ(ByteSearch::findForward(hay, folded, 0, false), 6);
    EXPECT_EQ(ByteSearch::findBackward(hay, folded, hay.size(), false), 11);
}

TEST(ByteSearchTest, CaseInsensitiveNeverFoldsNonAsciiLetters) {
    // É/é are one Unicode case pair and two unrelated byte pairs. Folding them
    // needs the decoded text, which a byte search does not have -- so it does
    // not happen, and the header says so rather than the behaviour surprising
    // somebody.
    const QByteArray hay = QStringLiteral("É").toUtf8();
    const ByteSearch::Needle folded =
        text(QStringLiteral("é"), kUtf8, ByteSearch::CaseFolding::AsciiInsensitive);
    ASSERT_TRUE(folded.valid);
    EXPECT_EQ(ByteSearch::findForward(hay, folded, 0, true), ByteSearch::kNotFound);
}

TEST(ByteSearchTest, CaseInsensitiveIsDeclinedRatherThanFakedForUtf16) {
    // UTF-16 does not put ASCII at its own byte value, so a byte-level fold
    // table describes nothing. The needle stays usable and says why.
    const ByteSearch::Needle needle =
        text(QStringLiteral("case"), kUtf16, ByteSearch::CaseFolding::AsciiInsensitive);
    ASSERT_TRUE(needle.valid);
    EXPECT_EQ(needle.folding, ByteSearch::CaseFolding::Exact);
    EXPECT_FALSE(needle.note.isEmpty());
    EXPECT_EQ(ByteSearch::foldSupport(kUtf16), ByteSearch::FoldSupport::Unsupported);
}

TEST(ByteSearchTest, FoldSupportIsProbedFromTheCodecNotAssumed) {
    EXPECT_EQ(ByteSearch::foldSupport(kUtf8), ByteSearch::FoldSupport::Full);
    EXPECT_EQ(ByteSearch::foldSupport(QByteArrayLiteral("ISO-8859-1")),
              ByteSearch::FoldSupport::Full);

    // GB18030, Big5 and Shift-JIS all use ASCII-letter byte values as trail
    // bytes, so folding stays available but is flagged. Skipped rather than
    // failed where the Qt build has no such codec.
    for (const QByteArray &name : {QByteArrayLiteral("GB18030"), QByteArrayLiteral("Big5"),
                                   QByteArrayLiteral("Shift-JIS")}) {
        if (!QTextCodec::codecForName(name))
            continue;
        EXPECT_EQ(ByteSearch::foldSupport(name), ByteSearch::FoldSupport::AsciiOnlyLossy)
            << name.constData();
        const ByteSearch::Needle needle =
            text(QStringLiteral("case"), name, ByteSearch::CaseFolding::AsciiInsensitive);
        ASSERT_TRUE(needle.valid) << name.constData();
        EXPECT_EQ(needle.folding, ByteSearch::CaseFolding::AsciiInsensitive);
        EXPECT_FALSE(needle.note.isEmpty()) << name.constData();
    }
}

TEST(ByteSearchTest, AnUnknownEncodingIsRefusedByName) {
    const ByteSearch::Needle needle = text(QStringLiteral("x"), QByteArrayLiteral("NOT-A-CODEC"));
    EXPECT_FALSE(needle.valid);
    EXPECT_TRUE(needle.error.contains(QLatin1String("NOT-A-CODEC")));
}
