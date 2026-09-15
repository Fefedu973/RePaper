#include "RichDocument.h"
#include "RichMessage.h"
#include "MarkdownScanner.h"
#include "MathEngine.h"
#include <QAbstractTextDocumentLayout>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPainter>
#include <QQmlContext>
#include <QQuickView>
#include <QQuickItemGrabResult>
#include <QSignalSpy>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextTable>

class RichTest : public QObject {
    Q_OBJECT
private slots:
    void coldFirstDocumentTypesetsMath();
    void markdownStructure();
    void codeStylingPreservesPrecedingList();
    void mathDelimiters();
    void formulasAndFallback();
    void securityAndLimits();
    void streamingAndNativePreview();
};
void RichTest::coldFirstDocumentTypesetsMath() {
    // Runs before any other formula. Initialization belongs to the production
    // document constructor; the test does not manually warm a cache or retry.
    QElapsedTimer startup; startup.start();
    rmchat::RichDocument doc;
    qInfo() << "Production math initialization ms:" << startup.elapsed();
    doc.setSource("Premier rendu : \\(x^2+y_1\\) et \\(\\frac{1}{\\sqrt{2}}\\).", 700, 24, Qt::black);
    QVERIFY(doc.hasMath()); QCOMPARE(doc.mathErrorCount(), 0);
    QCOMPARE(doc.toPlainText().count(QChar::ObjectReplacementCharacter), 2);
}
static QString fixture() {
    return QString::fromUtf8(R"MD(# Comprendre une fonction

Le **Markdown** reste lisible, avec *emphase*, ~~texte barré~~ et `code inline`.

## Une démarche en deux étapes

1. Observer le domaine.
2. Calculer la dérivée : \(f'(x)=2x\).
   - Comparer les valeurs.
   - Vérifier les unités.

> Une citation peut contenir une formule : $E=mc^2$.

| Notion | Expression |
| :--- | :--- |
| Fraction | $\frac{1}{2}$ |
| Racine | $\sqrt{x^2+1}$ |

```python
def carre(x):
    return x ** 2  # $ceci reste du code$
```

### Calcul intégral

\[
\int_0^1 x^2\,dx = \frac{1}{3},\qquad \sum_{k=1}^{n} k=\frac{n(n+1)}{2}
\]

$$\begin{pmatrix} a & b \\ c & d \end{pmatrix}\begin{pmatrix} x \\ y \end{pmatrix}=\begin{pmatrix} ax+by \\ cx+dy \end{pmatrix}$$

\[\begin{aligned} f(x)&=x^2+2x+1 \\ &= (x+1)^2 \end{aligned}\]

[Documentation](https://example.org/cours) · les liens s’ouvrent sur action explicite.
)MD");
}
void RichTest::markdownStructure() {
    rmchat::RichDocument doc;
    doc.setSource(fixture(), 832, 21, Qt::black);
    QVERIFY(doc.toPlainText().contains("Comprendre une fonction"));
    QVERIFY(!doc.toPlainText().contains("**Markdown**"));
    QVERIFY(doc.toPlainText().contains("$ceci reste du code$"));
    int bold = 0, headings = 0, lists = 0, anchors = 0, code = 0, tables = 0;
    QSet<int> mathIndices;
    for (auto block = doc.begin(); block.isValid(); block = block.next()) {
        headings += block.blockFormat().headingLevel() > 0;
        lists += block.textList() != nullptr;
        code += block.blockFormat().hasProperty(QTextFormat::BlockCodeFence);
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto format = it.fragment().charFormat();
            bold += format.fontWeight() >= QFont::Bold;
            anchors += format.isAnchor();
            if (format.objectType() == QTextFormat::UserObject + 81) mathIndices.insert(format.intProperty(QTextFormat::UserProperty + 81));
        }
    }
    for (auto *frame : doc.rootFrame()->childFrames()) {
        auto *table = qobject_cast<QTextTable *>(frame); tables += table != nullptr;
        if (table) for (int c = 0; c < table->columns(); ++c) {
            auto cursor = table->cellAt(0, c).firstCursorPosition(); const auto end = table->cellAt(0, c).lastCursorPosition().position();
            cursor.setPosition(end, QTextCursor::KeepAnchor);
            QCOMPARE(table->format().headerRowCount(), 1);
            QVERIFY(!cursor.selectedText().startsWith(QChar::ParagraphSeparator));
            QVERIFY(cursor.charFormat().fontWeight() >= QFont::Bold);
        }
    }
    QVERIFY(bold > 0); QVERIFY(headings >= 3); QVERIFY(lists >= 4); QVERIFY(anchors > 0); QVERIFY(code > 0); QCOMPARE(tables, 1);
    QVERIFY(doc.hasMath()); QCOMPARE(doc.mathErrorCount(), 0); QVERIFY(!doc.truncated());
    QCOMPARE(mathIndices.size(), 7);
    QVERIFY(doc.size().height() > 500);
    const auto wideHeight = doc.size().height();
    doc.setSource(fixture(), 360, 21, Qt::black); QVERIFY(doc.size().height() > wideHeight);
}
void RichTest::codeStylingPreservesPrecedingList() {
    rmchat::RichDocument doc;
    doc.setSource("> Une citation\n\n- Premier élément\n- Second élément\n\n```cpp\nauto valeur = 42;\n```\n\nAprès le code", 680, 21, Qt::black);
    int listItems = 0;
    for (auto block = doc.begin(); block.isValid(); block = block.next()) {
        if (block.text().contains("élément")) {
            ++listItems;
            QVERIFY(block.textList());
            QVERIFY(!block.blockFormat().hasProperty(QTextFormat::BlockCodeFence));
            QCOMPARE(block.blockFormat().background().style(), Qt::NoBrush);
        }
        if (block.text() == "Après le code") QVERIFY(!block.blockFormat().hasProperty(QTextFormat::BlockCodeFence));
    }
    QCOMPARE(listItems, 2);
}
void RichTest::mathDelimiters() {
    const auto scan = rmchat::scanMarkdown(QString::fromUtf8(R"MD(Inline $x^2$ et \(\alpha+\beta\).
$$\frac{a}{b}$$
\[\sqrt{x}\]
`$code$` et ``\(code\)``.
~~~tex
$fence$ \[fence\]
~~~
    $indent$
Échappé \$pasmath\$ ; prix 5 $ et 10 $.
)MD"));
    QCOMPARE(scan.math.size(), 4);
    QVERIFY(scan.markdown.contains("$fence$")); QVERIFY(scan.markdown.contains("$indent$"));
    QVERIFY(scan.markdown.contains("$code$")); QVERIFY(scan.markdown.contains("5 $ et 10 $"));
    QCOMPARE(rmchat::scanMarkdown("- ```tex\n  $code$\n  ```\n\n$x$").math.size(), 1);
    const auto partial = rmchat::scanMarkdown("Avant \\(\\frac{1}{");
    QCOMPARE(partial.math.size(), 1); QVERIFY(partial.math.first().incomplete);
}
void RichTest::formulasAndFallback() {
    for (const auto &source : {QStringLiteral("x^2+y_1"), QStringLiteral("\\frac{1}{\\sqrt{2}}"),
        QStringLiteral("\\int_0^1 x^2\\,dx"), QStringLiteral("\\sum_{i=1}^{n}i"),
        QStringLiteral("\\alpha+\\beta=\\pi"), QStringLiteral("\\begin{pmatrix}1&2\\\\3&4\\end{pmatrix}"),
        QStringLiteral("\\begin{aligned}x&=1\\\\y&=2\\end{aligned}")}) {
        const auto math = rmchat::MathEngine::render(source, true, 24, 700, Qt::black);
        QVERIFY2(math.valid(), qPrintable(source));
        int visiblePixels = 0;
        for (int y = 0; y < math.pixels.height(); ++y) for (int x = 0; x < math.pixels.width(); ++x)
            visiblePixels += qAlpha(math.pixels.pixel(x, y)) > 100;
        QVERIFY(visiblePixels > 20);
    }
    rmchat::RichDocument doc;
    doc.setSource("Avant \\(\\commandeInconnue{1}\\) après", 500, 21, Qt::black);
    QCOMPARE(doc.mathErrorCount(), 1); QVERIFY(doc.toPlainText().contains("\\commandeInconnue{1}"));
    doc.setSource("Avant \\(\\frac{1}{", 500, 21, Qt::black);
    QCOMPARE(doc.mathErrorCount(), 0); QVERIFY(doc.toPlainText().contains("\\(\\frac{1}{"));
    doc.setSource("Avant \\(\\frac{1}{2}\\) après", 500, 21, Qt::black);
    QVERIFY(doc.hasMath()); QCOMPARE(doc.mathErrorCount(), 0);
}
void RichTest::securityAndLimits() {
    rmchat::RichDocument doc;
    doc.setSource("<b>HTML brut</b> ![distant](https://127.0.0.1/private.png) ![local](file:///etc/passwd) [dangereux](javascript:alert(1))", 600, 21, Qt::black);
    QVERIFY(doc.toPlainText().contains("<b>HTML brut</b>"));
    QVERIFY(doc.toPlainText().contains("[Image : distant]"));
    for (auto block = doc.begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) QVERIFY(!it.fragment().charFormat().isImageFormat());
    QVERIFY(!rmchat::RichDocument::isSafeLink(QUrl("file:///etc/passwd")));
    QVERIFY(!rmchat::RichDocument::isSafeLink(QUrl("javascript:alert(1)")));
    QVERIFY(!rmchat::RichDocument::isSafeLink(QUrl("https://name:password@example.org")));
    QVERIFY(rmchat::RichDocument::isSafeLink(QUrl("https://example.org/course")));
    for (const auto &source : {"\\input{/etc/passwd}", "\\newcommand{\\x}{x}", "\\DeclareMathSizes{0}{1}{1}{1}",
        "\\magnification{999999}", "\\longdiv{-9223372036854775808}{-1}", "\\Roman{2147483647}", "\\XML{x}"})
        QVERIFY(!rmchat::MathEngine::accepts(QString::fromLatin1(source)));
    QVERIFY(!rmchat::MathEngine::accepts(QString(33, '{') + "x" + QString(33, '}')));
    QVERIFY(!rmchat::MathEngine::accepts(QString(8193, 'x')));
    QElapsedTimer timer; timer.start();
    QString many;
    for (int i = 0; i < 100; ++i) many += "\\(\\badCommand{" + QString::number(i) + "}\\) ";
    doc.setSource(many, 600, 21, Qt::black);
    QCOMPARE(doc.mathErrorCount(), 100);
    qInfo() << "100 invalid formulas ms:" << timer.elapsed();
    // QEMU translates Qt's entire document layout, so its wall-clock latency is
    // not a tablet benchmark. All production parser deadlines stay unchanged.
    QVERIFY(timer.elapsed() < (qEnvironmentVariableIntValue("RMCHAT_TEST_EMULATED") == 1 ? 15000 : 1500));
    QVERIFY(!rmchat::MathEngine::render("\\begin{array}{*{2147483647}{c}}x\\end{array}", true, 21, 600, Qt::black).valid());
    doc.setSource(QString(140000, 'x'), 600, 21, Qt::black); QVERIFY(doc.truncated());
    QVERIFY(doc.size().height() <= 20000);
    doc.setSource(QStringLiteral("ligne avec une phrase\n\n").repeated(500), 1600, 48, Qt::black, 4);
    QVERIFY(doc.truncated()); QVERIFY(doc.size().height() * 1600 * 16 <= 4 * 1024 * 1024);
}
void RichTest::streamingAndNativePreview() {
    rmchat::registerRichTypes();
    QQuickView view;
    view.rootContext()->setContextProperty("previewText", fixture());
    view.setSource(QUrl("qrc:/rmchat/rich-tests/Preview.qml"));
    QCOMPARE(view.status(), QQuickView::Ready);
    view.show(); QTest::qWait(160);
    auto *message = view.rootObject()->findChild<rmchat::RichMessage *>("richPreview");
    QVERIFY(message); QVERIFY(message->hasMath()); QCOMPARE(message->mathErrorCount(), 0);
    QVERIFY(message->contentHeight() > 500);
    const auto output = qEnvironmentVariable("RMCHAT_RICH_TEST_OUTPUT");
    if (!output.isEmpty()) {
        QDir().mkpath(output);
        auto grab = view.rootObject()->grabToImage();
        QSignalSpy ready(grab.data(), &QQuickItemGrabResult::ready);
        QVERIFY(ready.wait(1500)); QVERIFY(!grab->image().isNull());
        QVERIFY(grab->image().save(output + "/native-rich-preview.png"));
        rmchat::RichDocument doc; doc.setSource(fixture(), 832, 21, Qt::black);
        QImage image(900, qCeil(doc.size().height()) + 48, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::white);
        QPainter painter(&image); painter.translate(34, 24); doc.drawContents(&painter); painter.end();
        QVERIFY(image.save(output + "/markdown-latex-document.png"));
        for (int width : {640, 1080}) {
            doc.setSource(fixture(), width - 68, 21, Qt::black);
            QImage sized(width, qCeil(doc.size().height()) + 48, QImage::Format_ARGB32_Premultiplied); sized.fill(Qt::white);
            QPainter sizedPainter(&sized); sizedPainter.translate(34, 24); doc.drawContents(&sizedPainter); sizedPainter.end();
            QVERIFY(sized.save(output + QString("/markdown-latex-%1.png").arg(width)));
        }
    }
    QSignalSpy changed(message, &rmchat::RichMessage::layoutChanged);
    for (int i = 0; i < 80; ++i) message->setText("Étape " + QString::number(i) + " \\(\\frac{1}{");
    QTest::qWait(100); QVERIFY(changed.count() <= 2); QVERIFY(!message->hasMath()); QCOMPARE(message->mathErrorCount(), 0);
    message->setText("Terminé \\(\\frac{1}{2}\\)"); QTest::qWait(100);
    QVERIFY(message->hasMath()); QCOMPARE(message->mathErrorCount(), 0);
}
QTEST_MAIN(RichTest)
#include "RichTest.moc"
