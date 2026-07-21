#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QTemporaryDir>

#include "OfficeConverter.h"

// isOfficeFile / kindFor are pure string classification and need no QApplication.

TEST(OfficeConverterTest, IsOfficeFileRecognizesSupportedExtensionsCaseInsensitively) {
    EXPECT_TRUE(OfficeConverter::isOfficeFile("report.docx"));
    EXPECT_TRUE(OfficeConverter::isOfficeFile("report.DOCX"));
    EXPECT_TRUE(OfficeConverter::isOfficeFile("legacy.doc"));
    EXPECT_TRUE(OfficeConverter::isOfficeFile("slides.pptx"));
    EXPECT_TRUE(OfficeConverter::isOfficeFile("slides.PPT"));
    EXPECT_TRUE(OfficeConverter::isOfficeFile("sheet.xlsx"));
    EXPECT_TRUE(OfficeConverter::isOfficeFile("sheet.Xls"));
}

TEST(OfficeConverterTest, IsOfficeFileRejectsUnsupportedExtensions) {
    EXPECT_FALSE(OfficeConverter::isOfficeFile("notes.txt"));
    EXPECT_FALSE(OfficeConverter::isOfficeFile("image.png"));
    EXPECT_FALSE(OfficeConverter::isOfficeFile("archive.zip"));
    EXPECT_FALSE(OfficeConverter::isOfficeFile("no_extension"));
}

TEST(OfficeConverterTest, KindForReturnsDocumentForWordAndPowerPoint) {
    EXPECT_EQ(OfficeConverter::kindFor("a.doc"), OfficeConverter::Kind::Document);
    EXPECT_EQ(OfficeConverter::kindFor("a.docx"), OfficeConverter::Kind::Document);
    EXPECT_EQ(OfficeConverter::kindFor("a.ppt"), OfficeConverter::Kind::Document);
    EXPECT_EQ(OfficeConverter::kindFor("a.pptx"), OfficeConverter::Kind::Document);
}

TEST(OfficeConverterTest, KindForReturnsSpreadsheetForExcel) {
    EXPECT_EQ(OfficeConverter::kindFor("a.xls"), OfficeConverter::Kind::Spreadsheet);
    EXPECT_EQ(OfficeConverter::kindFor("a.xlsx"), OfficeConverter::Kind::Spreadsheet);
}

TEST(OfficeConverterTest, KindForReturnsNoneForUnsupportedFiles) {
    EXPECT_EQ(OfficeConverter::kindFor("a.txt"), OfficeConverter::Kind::None);
    EXPECT_EQ(OfficeConverter::kindFor("a"), OfficeConverter::Kind::None);
}

TEST(OfficeConverterTest, ConvertFailsForUnsupportedExtension) {
    const OfficeConverter::Result result = OfficeConverter::convert(QStringLiteral("/tmp/whatever.txt"));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.kind, OfficeConverter::Kind::None);
    EXPECT_FALSE(result.error.isEmpty());
}

// Forces resolveBinary()/isAvailable()/convert() down the "binary not found"
// path by pointing TTC_OFFICE_OXIDE at a bogus file and hiding PATH behind an
// empty directory, so these tests don't depend on whether office_oxide
// happens to be installed on the machine running them (it isn't, in CI).
class OfficeConverterUnavailableTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_hadOverride = qEnvironmentVariableIsSet("TTC_OFFICE_OXIDE");
        if (m_hadOverride)
            m_previousOverride = qgetenv("TTC_OFFICE_OXIDE");
        m_hadPath = qEnvironmentVariableIsSet("PATH");
        if (m_hadPath)
            m_previousPath = qgetenv("PATH");
        m_hadHome = qEnvironmentVariableIsSet("HOME");
        if (m_hadHome)
            m_previousHome = qgetenv("HOME");

        ASSERT_TRUE(m_emptyDir.isValid());
        qputenv("TTC_OFFICE_OXIDE", QByteArray("/nonexistent/does-not-exist/office_oxide"));
        qputenv("PATH", m_emptyDir.path().toUtf8());
        // resolveBinary() also probes ~/.local/bin and ~/.cargo/bin, so point
        // HOME at the empty dir too -- otherwise a real office-oxide installed
        // under the developer's home would make these "unavailable" tests flaky.
        qputenv("HOME", m_emptyDir.path().toUtf8());
    }

    void TearDown() override {
        if (m_hadOverride)
            qputenv("TTC_OFFICE_OXIDE", m_previousOverride);
        else
            qunsetenv("TTC_OFFICE_OXIDE");
        if (m_hadHome)
            qputenv("HOME", m_previousHome);
        else
            qunsetenv("HOME");
        if (m_hadPath)
            qputenv("PATH", m_previousPath);
        else
            qunsetenv("PATH");
    }

    QTemporaryDir m_emptyDir;
    bool m_hadOverride = false;
    QByteArray m_previousOverride;
    bool m_hadPath = false;
    QByteArray m_previousPath;
    bool m_hadHome = false;
    QByteArray m_previousHome;
};

TEST_F(OfficeConverterUnavailableTest, IsAvailableIsFalseWhenBinaryCannotBeResolved) {
    EXPECT_TRUE(OfficeConverter::resolveBinary().isEmpty());
    EXPECT_FALSE(OfficeConverter::isAvailable());
}

TEST_F(OfficeConverterUnavailableTest, ConvertFailsWithHelpfulErrorWhenBinaryUnavailable) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.path() + QStringLiteral("/doc.docx");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("not a real docx, just needs to exist on disk");
    file.close();

    const OfficeConverter::Result result = OfficeConverter::convert(path);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.kind, OfficeConverter::Kind::Document);
    EXPECT_FALSE(result.error.isEmpty());
    EXPECT_TRUE(result.error.contains(QStringLiteral("office_oxide"), Qt::CaseInsensitive))
        << result.error.toStdString();
}

TEST_F(OfficeConverterUnavailableTest, ConvertFailsForNonexistentFile) {
    const OfficeConverter::Result result =
        OfficeConverter::convert(QStringLiteral("/nonexistent/path/report.docx"));
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.isEmpty());
}
