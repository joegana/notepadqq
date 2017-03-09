#include "include/EditorNS/editor.h"
#include "include/notepadqq.h"
#include "include/nqqsettings.h"
#include <QVBoxLayout>
#include <QMessageBox>
#include <QDir>
#include <QEventLoop>
#include <QUrlQuery>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <QWebEngineSettings>
#include <QtWebChannel/QWebChannel>
#include <QJSEngine>

namespace EditorNS
{

    QQueue<Editor*> Editor::m_editorBuffer = QQueue<Editor*>();

    Editor::Editor(QWidget *parent) :
        QWidget(parent)
    {

        QString themeName = NqqSettings::getInstance().Appearance.getColorScheme();
        if (themeName == "")
            themeName = "default";

        fullConstructor(themeFromName(themeName));
    }

    Editor::Editor(const Theme &theme, QWidget *parent) :
        QWidget(parent)
    {
        fullConstructor(theme);
    }

    void Editor::fullConstructor(const Theme &theme)
    {
        initJsProxy();
        m_webView = new CustomQWebView(this);

        QUrl url = QUrl("file://" + Notepadqq::editorPath());

        m_webView->connectJavaScriptObject("cpp_ui_driver", m_jsToCppProxy);
        
        m_webView->page()->load(url);
        m_webView->page()->setBackgroundColor(Qt::transparent);

        initContextMenu();
        
        QWebEngineSettings *pageSettings = m_webView->page()->settings();
        pageSettings->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, true);

        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(0);
        m_layout->addWidget(m_webView, 1);
        setLayout(m_layout);
        
        connect(m_webView->page(),
                &QWebEnginePage::loadFinished,
                this,
                &Editor::on_javaScriptWindowObjectCleared);
        connect(m_webView, &CustomQWebView::mouseWheel, this, &Editor::mouseWheel);
        connect(m_webView, &CustomQWebView::urlsDropped, this, &Editor::urlsDropped);
    }


    void Editor::initJsProxy()
    {
        m_jsToCppProxy = new JsToCppProxy(this);
        connect(m_jsToCppProxy, &JsToCppProxy::replyReady, &m_processLoop, &QEventLoop::quit);
        connect(m_jsToCppProxy,
                &JsToCppProxy::editorEvent, 
                this, 
                &Editor::on_proxyMessageReceived
                );
    }

    void Editor::initContextMenu()
    {
        m_webView->setContextMenuPolicy(
                Qt::ContextMenuPolicy::ActionsContextMenu);
        QWebEnginePage* page = m_webView->page();
        m_webView->addAction(page->action(QWebEnginePage::Cut));
        m_webView->addAction(page->action(QWebEnginePage::Copy));
        m_webView->addAction(page->action(QWebEnginePage::Paste));
        m_webView->addAction(page->action(QWebEnginePage::SelectAll));
        page->action(QWebEnginePage::Undo)->setEnabled(false);
        page->action(QWebEnginePage::Redo)->setEnabled(false);
    }


    QSharedPointer<Editor> Editor::getNewEditor(QWidget *parent)
    {
        return QSharedPointer<Editor>(getNewEditorUnmanagedPtr(parent), &Editor::deleteLater);
    }

    Editor *Editor::getNewEditorUnmanagedPtr(QWidget *parent)
    {
        Editor *out;

        if (m_editorBuffer.length() == 0) {
            m_editorBuffer.enqueue(new Editor());
            out = new Editor();
        } else if (m_editorBuffer.length() == 1) {
            m_editorBuffer.enqueue(new Editor());
            out = m_editorBuffer.dequeue();
        } else {
            out = m_editorBuffer.dequeue();
        }

        out->setParent(parent);
        return out;
    }

    void Editor::addEditorToBuffer(const int howMany)
    {
        for (int i = 0; i < howMany; i++)
            m_editorBuffer.enqueue(new Editor());
    }

    void Editor::invalidateEditorBuffer()
    {
        m_editorBuffer.clear();
    }

    void Editor::waitAsyncLoad()
    {
        if (!m_loaded) {
            QEventLoop loop;
            connect(this, &Editor::editorReady, &loop, &QEventLoop::quit);
            // Block until a J_EVT_READY message is received
            loop.exec();
        }
    }

    void Editor::on_javaScriptWindowObjectCleared()
    {
        m_webView->connectJavaScriptObject("cpp_ui_driver", m_jsToCppProxy);
    }

    void Editor::on_languageChange()
    {
        QVariantMap data = m_jsToCppProxy->getRawValue("language").toMap();
        QString id = data.value("id").toString();
        QString lang = data.value("lang").toMap().value("name").toString();
        emit currentLanguageChanged(id, lang);
    }

    void Editor::on_proxyMessageReceived(QString msg, QVariant data)
    {
        emit messageReceived(msg, data);

        if(msg == "J_EVT_READY") {
            m_loaded = true;
            emit editorReady();
        } else if(msg == "J_EVT_CONTENT_CHANGED")
            emit contentChanged();
        else if(msg == "J_EVT_CLEAN_CHANGED")
            emit cleanChanged(data.toBool());
        else if(msg == "J_EVT_CURSOR_ACTIVITY")
            emit cursorActivity();
        else if(msg == "J_EVT_GOT_FOCUS")
            emit gotFocus();
        else if(msg == "J_EVT_CURRENT_LANGUAGE_CHANGED") {
            QVariantMap lang = data.toMap();
            QString id = lang.value("id").toString();
            QString name = lang.value("lang").toMap().value("name").toString();
            if (!m_customIndentationMode) {
                setIndentationMode(id);
            }
            emit currentLanguageChanged(id, name);
        }else if(msg == "J_EVT_FILE_LOADED")
        {
            emit fileLoaded();
        }
    }

    void Editor::setFocus()
    {
        m_webView->setFocus();
        sendMessage("C_CMD_SET_FOCUS");
    }

    void Editor::clearFocus()
    {
        m_webView->clearFocus();
        sendMessage("C_CMD_BLUR");
    }

    /**
     * Automatically converts local relative file names to absolute ones.
     */
    void Editor::setFileName(const QUrl &filename)
    {
        QUrl old = m_fileName;
        QUrl newUrl = filename;

        if (newUrl.isLocalFile())
            newUrl = QUrl::fromLocalFile(QFileInfo(filename.toLocalFile()).absoluteFilePath());

        m_fileName = newUrl;
        emit fileNameChanged(old, newUrl);
    }

    /**
     * Always returns an absolute url.
     */
    QUrl Editor::fileName() const
    {
        return m_fileName;
    }

    bool Editor::isClean()
    {
        bool clean;
        m_jsToCppProxy->getValue("clean", clean);
        return clean;
    }

    void Editor::markClean()
    {
        sendMessage("C_CMD_MARK_CLEAN");
    }

    void Editor::markDirty()
    {
        sendMessage("C_CMD_MARK_DIRTY");
    }

    QList<QMap<QString, QString>> Editor::languages()
    {
        // TODO: Cache this data.
        QJSEngine engine;
        QFileInfo fileInfo(Notepadqq::editorPath());
        QString fileName = fileInfo.absolutePath() + "/classes/Languages.js";
        QFile scriptFile(fileName);
        scriptFile.open(QIODevice::ReadOnly);
        QTextStream stream(&scriptFile);
        QString contents = stream.readAll();
        scriptFile.close();

        QJSValue result = engine.evaluate(contents, fileName);

        if(result.isError()) {
            qDebug() << "Failed to load languages file.";
            return QList<QMap<QString, QString>>();
        }

        QMap<QString, QVariant> languages =
            engine.evaluate("Languages.languages", fileName).toVariant().toMap();

        QList<QMap<QString, QString>> langs;

        QMap<QString, QVariant>::iterator lang;
        for (lang = languages.begin(); lang != languages.end(); ++lang) {
            QMap<QString, QVariant> mode = lang.value().toMap();

            QMap<QString, QString> newMode;
            newMode.insert("id", lang.key());
            newMode.insert("name", mode.value("name").toString());
            newMode.insert("mime", mode.value("mime").toString());
            newMode.insert("mode", mode.value("mode").toString());
            langs.append(newMode);
        }
        return langs;
    }

    QString Editor::getLanguage(const QString& val)
    {
        QVariantMap data = m_jsToCppProxy->getRawValue("language").toMap();
        if ( val == "id" ) {
            return data.value("id").toString();
        }
        return data.value("lang").toMap().value(val).toString();
    }

    void Editor::setLanguage(const QString &language)
    {
        sendMessage("C_CMD_SET_LANGUAGE", language);
        if (!m_customIndentationMode)
            setIndentationMode(language);
    }

    void Editor::setLanguageFromFileName(QString fileName)
    {
        sendMessage("C_FUN_SET_LANGUAGE_FROM_FILENAME", fileName);
    }

    void Editor::setLanguageFromFileName()
    {
        setLanguageFromFileName(fileName().toString());
    }

    void Editor::setIndentationMode(QString language)
    {
        NqqSettings& s = NqqSettings::getInstance();

        if (s.Languages.getUseDefaultSettings(language))
            language = "default";

        setIndentationMode(!s.Languages.getIndentWithSpaces(language),
                            s.Languages.getTabSize(language));
    }

    void Editor::setIndentationMode(const bool useTabs, const int size)
    {
        QMap<QString, QVariant> data;
        data.insert("useTabs", useTabs);
        data.insert("size", size);
        sendMessage("C_CMD_SET_INDENTATION_MODE", data);
    }

    Editor::IndentationMode Editor::indentationMode()
    {
        QPair<int, int> indent;
        m_jsToCppProxy->getValue("indentMode", indent);
        IndentationMode out;
        out.useTabs = indent.first;
        out.size = indent.second;
        return out;
    }

    void Editor::setCustomIndentationMode(const bool useTabs, const int size)
    {
        m_customIndentationMode = true;
        setIndentationMode(useTabs, size);
    }

    void Editor::setCustomIndentationMode(const bool useTabs)
    {
        m_customIndentationMode = true;
        setIndentationMode(useTabs, 0);
    }

    void Editor::clearCustomIndentationMode()
    {
        m_customIndentationMode = false;
        setIndentationMode(getLanguage());
    }

    bool Editor::isUsingCustomIndentationMode() const
    {
        return m_customIndentationMode;
    }

    void Editor::setSmartIndent(bool enabled)
    {
        sendMessage("C_CMD_SET_SMART_INDENT", enabled);
    }

    void Editor::setValue(const QString &value)
    {
        sendMessage("C_CMD_SET_VALUE", value);
    }

    QString Editor::value()
    {
        return sendMessageWithResult("C_FUN_GET_VALUE").toString();
    }

    bool Editor::fileOnDiskChanged() const
    {
        return m_fileOnDiskChanged;
    }

    void Editor::setFileOnDiskChanged(bool fileOnDiskChanged)
    {
        m_fileOnDiskChanged = fileOnDiskChanged;
    }

    QString Editor::jsStringEscape(QString str) const {
        return str.replace("\\", "\\\\")
                .replace("'", "\\'")
                .replace("\"", "\\\"")
                .replace("\n", "\\n")
                .replace("\r", "\\r")
                .replace("\t", "\\t")
                .replace("\b", "\\b");
    }

    void Editor::sendMessage(const QString &msg, const QVariant &data)
    {
//        waitAsyncLoad();
        m_jsToCppProxy->sendMsg(jsStringEscape(msg), data);
    }

    QVariant Editor::sendMessageWithResult(const QString &msg, const QVariant &data)
    {
        qDebug() << "Getting result for: " << msg;
//        waitAsyncLoad();
        if (m_processLoop.isRunning())
            throw std::runtime_error("m_processLoop must never be running at this point. Did this function get called from another thread?");

        emit m_jsToCppProxy->sendMsg(jsStringEscape(msg), data);
        m_processLoop.exec();
        return m_jsToCppProxy->getResult();
    }

    void Editor::setZoomFactor(const qreal &factor)
    {
        qreal normFact = factor;
        if (normFact > 14) normFact = 14;
        else if (normFact < 0.10) normFact = 0.10;

        m_webView->setZoomFactor(normFact);
    }

    qreal Editor::zoomFactor() const
    {
        return m_webView->zoomFactor();
    }

    int Editor::textLength()
    {
        int charCount;
        m_jsToCppProxy->getValue("charCount", charCount);
        return charCount;
    }

    void Editor::setSelectionsText(const QStringList &texts, selectMode mode)
    {
        QString modeStr = "";
        if (mode == selectMode_cursorAfter)
            modeStr = "after";
        else if (mode == selectMode_cursorBefore)
            modeStr = "before";
        else
            modeStr = "selected";

        QVariantMap data;
        data.insert("text", texts);
        data.insert("select", modeStr);

        sendMessage("C_CMD_SET_SELECTIONS_TEXT", data);
    }

    void Editor::setSelectionsText(const QStringList &texts)
    {
        setSelectionsText(texts, selectMode_cursorAfter);
    }

    void Editor::insertBanner(QWidget *banner)
    {
        m_layout->insertWidget(0, banner);
    }

    void Editor::removeBanner(QWidget *banner)
    {
        if (banner != m_webView && m_layout->indexOf(banner) >= 0) {
            m_layout->removeWidget(banner);
            emit bannerRemoved(banner);
        }
    }

    void Editor::removeBanner(QString objectName)
    {
        QList<QWidget *> list = findChildren<QWidget *>(objectName);
        for (int i = 0; i < list.length(); i++) {
            removeBanner(list[i]);
        }
    }

    void Editor::setLineWrap(const bool wrap)
    {
        sendMessage("C_CMD_SET_LINE_WRAP", wrap);
    }

    void Editor::setEOLVisible(const bool showeol)
    {
        sendMessage("C_CMD_SHOW_END_OF_LINE",showeol);
    }

    void Editor::setWhitespaceVisible(const bool showspace)
    {
        sendMessage("C_CMD_SHOW_WHITESPACE",showspace);
    }

    QPair<int, int> Editor::cursorPosition()
    {
        QPair<int, int> cursor;
        m_jsToCppProxy->getValue("cursor", cursor);
        return cursor;
    }

    void Editor::setCursorPosition(const int line, const int column)
    {
        QList<QVariant> arg = QList<QVariant>({line, column});
        sendMessage("C_CMD_SET_CURSOR", QVariant(arg));
    }

    void Editor::setCursorPosition(const QPair<int, int> &position)
    {
        setCursorPosition(position.first, position.second);
    }

    void Editor::setCursorPosition(const Cursor &cursor)
    {
        setCursorPosition(cursor.line, cursor.column);
    }

    void Editor::setSelection(int fromLine, int fromCol, int toLine, int toCol)
    {
        QList<QVariant> arg = QList<QVariant>({fromLine, fromCol, toLine, toCol});
        sendMessage("C_CMD_SET_SELECTION", QVariant(arg));
    }

    QPair<int, int> Editor::scrollPosition()
    {
        QPair<int, int> scrollPosition;
        m_jsToCppProxy->getValue("scrollPosition", scrollPosition);
        return scrollPosition;
    }

    void Editor::setScrollPosition(const int left, const int top)
    {
        QList<QVariant> arg = QList<QVariant>({left, top});
        sendMessage("C_CMD_SET_SCROLL_POS", QVariant(arg));
    }

    void Editor::setScrollPosition(const QPair<int, int> &position)
    {
        setScrollPosition(position.first, position.second);
    }

    QString Editor::endOfLineSequence() const
    {
        return m_endOfLineSequence;
    }

    void Editor::setEndOfLineSequence(const QString &newLineSequence)
    {
        m_endOfLineSequence = newLineSequence;
    }

    void Editor::setFont(QString fontFamily, int fontSize, double lineHeight)
    {
        QMap<QString, QVariant> tmap;
        tmap.insert("family", fontFamily == nullptr ? "" : fontFamily);
        tmap.insert("size", QString::number(fontSize));
        tmap.insert("lineHeight", QString::number(lineHeight,'f',2));
        sendMessage("C_CMD_SET_FONT", tmap);
    }

    QTextCodec *Editor::codec() const
    {
        return m_codec;
    }

    void Editor::setCodec(QTextCodec *codec)
    {
        m_codec = codec;
    }

    bool Editor::bom() const
    {
        return m_bom;
    }

    void Editor::setBom(bool bom)
    {
        m_bom = bom;
    }

    Editor::Theme Editor::themeFromName(QString name)
    {
        Theme defaultTheme;
        defaultTheme.name = "default";
        defaultTheme.path = "";

        if (name == "default" || name == "")
            return defaultTheme;

        QFileInfo editorPath = QFileInfo(Notepadqq::editorPath());
        QDir bundledThemesDir = QDir(editorPath.absolutePath() + "/libs/codemirror/theme/");

        Theme t;
        QString themeFile = bundledThemesDir.filePath(name + ".css");
        if (QFile(themeFile).exists()) {
            t.name = name;
            t.path = themeFile;
        } else {
            t = defaultTheme;
        }

        return t;
    }

    QList<Editor::Theme> Editor::themes()
    {
        QFileInfo editorPath = QFileInfo(Notepadqq::editorPath());
        QDir bundledThemesDir = QDir(editorPath.absolutePath() + "/libs/codemirror/theme/");

        QStringList filters;
        filters << "*.css";
        bundledThemesDir.setNameFilters(filters);

        QStringList themeFiles = bundledThemesDir.entryList();

        QList<Theme> out;
        for (QString themeStr : themeFiles) {
            QFileInfo theme = QFileInfo(themeStr);
            QString nameWithoutExt = theme.fileName()
                    .replace(QRegularExpression("\\.css$"), "");

            Theme t;
            t.name = nameWithoutExt;
            t.path = bundledThemesDir.filePath(themeStr);
            out.append(t);
        }

        return out;
    }

    void Editor::setTheme(Theme theme)
    {
        QMap<QString, QVariant> tmap;
        tmap.insert("name", theme.name == "" ? "default" : theme.name);
        tmap.insert("path", theme.path);
        sendMessage("C_CMD_SET_THEME", tmap);
    }

    QList<Editor::Selection> Editor::selections()
    {
        QList<Selection> out;

        QList<QVariant> sels = m_jsToCppProxy->getRawValue("selections").toList();
        for (int i = 0; i < sels.length(); i++) {
            QVariantMap selMap = sels[i].toMap();
            QVariantMap from = selMap.value("anchor").toMap();
            QVariantMap to = selMap.value("head").toMap();

            Selection sel;
            sel.from.line = from.value("line").toInt();
            sel.from.column = from.value("ch").toInt();
            sel.to.line = to.value("line").toInt();
            sel.to.column = to.value("ch").toInt();

            out.append(sel);
        }

        return out;
    }

    QStringList Editor::selectedTexts()
    {
        QStringList selectedTexts;
        m_jsToCppProxy->getValue("selectionsText", selectedTexts);
        return selectedTexts;
    }

    void Editor::setOverwrite(bool overwrite)
    {
        sendMessage("C_CMD_SET_OVERWRITE", overwrite);
    }

    void Editor::forceRender(QSize size)
    {
        // FIXME Not needed anymore?
/*
#ifndef USE_QTWEBENGINE
        QWebPage *page = m_webView->page();

        page->setViewportSize(size);

        QImage image(size.width(), size.height(), QImage::Format_Mono);
        QPainter painter(&image);

        page->mainFrame()->render(&painter);
#endif
*/
    }

    void Editor::setTabsVisible(bool visible)
    {
        sendMessage("C_CMD_SET_TABS_VISIBLE", visible);
    }

    Editor::IndentationMode Editor::detectDocumentIndentation(bool *found)
    {
        IndentationMode out;
        QPair<int, int> indent;
        bool _found = m_jsToCppProxy->getValue("detectedIndent", indent);

        if (found != nullptr) {
            *found = _found;
        }

        if (_found) {
            out.useTabs = indent.first;
            out.size = indent.second;
        }

        return out;
    }

    void Editor::print(QPrinter *printer)
    {
        sendMessage("C_CMD_DISPLAY_PRINT_STYLE");
        //m_webView->print(printer); // FIXME
        sendMessage("C_CMD_DISPLAY_NORMAL_STYLE");
    }

    QString Editor::getCurrentWord()
    {
        return sendMessageWithResult("C_FUN_GET_CURRENT_WORD").toString();
    }

    int Editor::lineCount()
    {
        int lineCount;
        m_jsToCppProxy->getValue("lineCount", lineCount);
        return lineCount;
    }

}
