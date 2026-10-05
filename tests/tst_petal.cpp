#include <QtTest>
#include "core/PetalDocument.h"

using namespace rose;

class PetalTests final : public QObject {
    Q_OBJECT
private slots:
    void renamePreservesOpaqueProperties();
    void textLiteralDoesNotCreateObjects();
    void truncatedDocumentReportsLocation();
    void documentRejectsForeignEditHandles();
    void rejectsMalformedLocalReference();
    void encodingIsExplicitAndLossless();
    void encodingRejectsTruncatedInputAndLossyUnicode();
    void byteViewsBorrowStorageAndHashEmbeddedNul();
    void deepNestingDoesNotUseCallStack();
    void overlappingEditsCannotCorruptSource();
};

void PetalTests::renamePreservesOpaqueProperties() {
    const QByteArray source = "(object Petal version 50 charSet 0)\r\n"
        "(object Class \"Order\" quid \"47209F1F003E\"\r\n"
        " documentation \"Text containing (object Class) and @1\"\r\n"
        " vendorExtension (\"UnknownKind\" 201))\r\n";
    auto parsed = PetalDocument::parse(source);
    QVERIFY(std::holds_alternative<PetalDocument>(parsed));
    const auto &document = std::get<PetalDocument>(parsed);
    const auto name = document.objectName("Class", "47209F1F003E");
    QVERIFY(name.has_value());
    QCOMPARE(name->text(), QStringLiteral("Order"));
    auto rewritten = document.replaceName(*name, "Pembayaran \"Tunai\"");
    QVERIFY(std::holds_alternative<QByteArray>(rewritten));
    auto reopened = PetalDocument::parse(std::get<QByteArray>(rewritten));
    QVERIFY(std::holds_alternative<PetalDocument>(reopened));
    const auto &after = std::get<PetalDocument>(reopened);
    const auto newName = after.objectName("Class", "47209F1F003E");
    QVERIFY(newName.has_value());
    QCOMPARE(newName->text(), QStringLiteral("Pembayaran \"Tunai\""));
    const auto documentation = after.objectProperty("Class", "47209F1F003E", "documentation");
    const auto extension = after.objectProperty("Class", "47209F1F003E", "vendorExtension");
    QVERIFY(documentation.has_value());
    QVERIFY(extension.has_value());
    QCOMPARE(documentation->rawValue(after), ByteView("\"Text containing (object Class) and @1\""));
    QCOMPARE(extension->rawValue(after), ByteView("(\"UnknownKind\" 201)"));
    QCOMPARE(after.bytes(), QByteArray(
        "(object Petal version 50 charSet 0)\r\n"
        "(object Class \"Pembayaran \\\"Tunai\\\"\" quid \"47209F1F003E\"\r\n"
        " documentation \"Text containing (object Class) and @1\"\r\n"
        " vendorExtension (\"UnknownKind\" 201))\r\n"));
    QCOMPARE(document.bytes(), source);
}

void PetalTests::textLiteralDoesNotCreateObjects() {
    const QByteArray source = "(object Petal version 50 charSet 0)\n"
        "(object Class \"Order\" quid \"47209F1F003E\" documentation (value Text\n"
        "|first line (object Class \"NotAnElement\")\n"
        "|second line @99\n"
        "))\n";
    auto parsed = PetalDocument::parse(source);
    QVERIFY(std::holds_alternative<PetalDocument>(parsed));
    const auto &document = std::get<PetalDocument>(parsed);
    QCOMPARE(document.objectsOfKind("Class").size(), 1);
    const auto documentation = document.objectProperty("Class", "47209F1F003E", "documentation");
    QVERIFY(documentation.has_value());
    QCOMPARE(documentation->rawValue(document), ByteView("(value Text\n|first line (object Class \"NotAnElement\")\n|second line @99\n)"));
}

void PetalTests::truncatedDocumentReportsLocation() {
    auto parsed = PetalDocument::parse("(object Petal version 50)\n(object Class \"Order\"\n");
    QVERIFY(std::holds_alternative<WorkspaceError>(parsed));
    const auto &error = std::get<WorkspaceError>(parsed);
    QCOMPARE(error.code, ErrorCode::InvalidSyntax);
    QCOMPARE(error.line, 2);
    QCOMPARE(error.column, 1);
}

void PetalTests::documentRejectsForeignEditHandles() {
    auto first = PetalDocument::parse("(object Class \"Order\" quid \"47209F1F003E\")");
    auto second = PetalDocument::parse("(object Class \"Other\" quid \"47209F1F003E\")");
    QVERIFY(std::holds_alternative<PetalDocument>(first));
    QVERIFY(std::holds_alternative<PetalDocument>(second));
    const auto &a = std::get<PetalDocument>(first);
    const auto &b = std::get<PetalDocument>(second);
    auto name = a.objectName("Class", "47209F1F003E");
    QVERIFY(name.has_value());
    const auto result = b.replaceName(*name, "WrongDocument");
    QVERIFY(std::holds_alternative<WorkspaceError>(result));
    QCOMPARE(std::get<WorkspaceError>(result).code, ErrorCode::InvalidCommand);
    QCOMPARE(b.objectName("Class", "47209F1F003E")->text(), QStringLiteral("Other"));
}

void PetalTests::rejectsMalformedLocalReference() {
    auto parsed = PetalDocument::parse("(object ClassView \"Class\" @oops location (1, 2))");
    QVERIFY(std::holds_alternative<WorkspaceError>(parsed));
    QCOMPARE(std::get<WorkspaceError>(parsed).code, ErrorCode::InvalidSyntax);
}

void PetalTests::encodingIsExplicitAndLossless() {
    const QByteArray source = "(object Class \"Caf\xe9\" quid \"47209F1F003E\")";
    auto unspecified = PetalDocument::parse(source);
    QVERIFY(std::holds_alternative<WorkspaceError>(unspecified));
    QCOMPARE(std::get<WorkspaceError>(unspecified).code, ErrorCode::UnsupportedEncoding);
    auto parsed = PetalDocument::parse(source, "Windows-1252");
    QVERIFY(std::holds_alternative<PetalDocument>(parsed));
    const auto &document = std::get<PetalDocument>(parsed);
    auto name = document.objectName("Class", "47209F1F003E");
    QVERIFY(name.has_value());
    QCOMPARE(name->text(), QStringLiteral("Café"));
    auto changed = document.replaceName(*name, QStringLiteral("Crème"));
    QVERIFY(std::holds_alternative<QByteArray>(changed));
    QCOMPARE(std::get<QByteArray>(changed), QByteArray("(object Class \"Cr\xe8me\" quid \"47209F1F003E\")"));
    auto unrepresentable = document.replaceName(*name, QString::fromUtf8("価格"));
    QVERIFY(std::holds_alternative<WorkspaceError>(unrepresentable));
    QCOMPARE(std::get<WorkspaceError>(unrepresentable).code, ErrorCode::UnsupportedEncoding);
}

void PetalTests::encodingRejectsTruncatedInputAndLossyUnicode() {
    for (const QByteArray suffix : {QByteArray("\xc3"), QByteArray("\xe2\x82"), QByteArray("\xf0\x9f\x92")}) {
        const auto parsed = PetalDocument::parse("(object Class \"Broken" + suffix + "\" quid \"47209F1F003E\")", "UTF-8");
        QVERIFY(std::holds_alternative<WorkspaceError>(parsed));
        QCOMPARE(std::get<WorkspaceError>(parsed).code, ErrorCode::UnsupportedEncoding);
    }
    for (const QByteArray codec : {QByteArray("UTF-8"), QByteArray("Windows-1252")}) {
        const auto parsed = PetalDocument::parse("(object Class \"Item\" quid \"47209F1F003E\")", codec);
        QVERIFY(std::holds_alternative<PetalDocument>(parsed));
        const auto &document = std::get<PetalDocument>(parsed);
        const auto name = document.objectName("Class", "47209F1F003E");
        QVERIFY(name);
        const auto edited = document.replaceName(*name, QString(QChar(0xd800)));
        QVERIFY(std::holds_alternative<WorkspaceError>(edited));
        QCOMPARE(std::get<WorkspaceError>(edited).code, ErrorCode::UnsupportedEncoding);
    }
    QVERIFY(std::holds_alternative<WorkspaceError>(
        PetalDocument::parse("(object Class \"Item\")", "UTF-16")));
}

void PetalTests::byteViewsBorrowStorageAndHashEmbeddedNul() {
    const QByteArray bytes("a\0b", 3);
    const ByteView view(bytes);
    QCOMPARE(view.data(), bytes.constData());
    QCOMPARE(view.sliced(1, 2).data(), bytes.constData() + 1);
    QCOMPARE(view.sliced(1, 2).toByteArray(), QByteArray("\0b", 2));
    const QByteArray equal("a\0b", 3), different("a\0c", 3);
    QHash<ByteView, int> index;
    index.insert(view, 7);
    QCOMPARE(index.value(ByteView(equal)), 7);
    QVERIFY(!index.contains(ByteView(different)));
    const auto parsed = PetalDocument::parse("(object Class \"Item\" quid \"47209F1F003E\")");
    QVERIFY(std::holds_alternative<PetalDocument>(parsed));
    const auto &document = std::get<PetalDocument>(parsed);
    const auto span = document.nodes().front().parts.front().span;
    QCOMPARE(document.raw(span).data(), document.bytes().constData() + span.offset);
}

void PetalTests::deepNestingDoesNotUseCallStack() {
    QByteArray source(20000, '(');
    source.append("\"leaf\"");
    source.append(QByteArray(20000, ')'));
    auto parsed = PetalDocument::parse(source);
    QVERIFY(std::holds_alternative<PetalDocument>(parsed));
    const auto &document = std::get<PetalDocument>(parsed);
    const auto &leaf = document.nodes().back();
    QCOMPARE(std::get<QString>(document.text(leaf.parts.front())), QStringLiteral("leaf"));
    QCOMPARE(document.raw(document.nodes().front().span), ByteView(source));
}

void PetalTests::overlappingEditsCannotCorruptSource() {
    auto parsed = PetalDocument::parse("(object Class \"Order\" quid \"47209F1F003E\")");
    QVERIFY(std::holds_alternative<PetalDocument>(parsed));
    const auto &document = std::get<PetalDocument>(parsed);
    const auto original = document.bytes();
    auto result = document.applyPatches({{{14, 5}, "X"}, {{16, 2}, "Y"}});
    QVERIFY(std::holds_alternative<WorkspaceError>(result));
    QCOMPARE(std::get<WorkspaceError>(result).code, ErrorCode::InvalidCommand);
    QCOMPARE(document.objectName("Class", "47209F1F003E")->text(), QStringLiteral("Order"));
    QCOMPARE(document.bytes(), original);
}

QTEST_GUILESS_MAIN(PetalTests)
#include "tst_petal.moc"
