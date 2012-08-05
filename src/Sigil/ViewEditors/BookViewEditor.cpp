/************************************************************************
**
**  Copyright (C) 2012 John Schember <john@nachtimwald.com>
**
**  This file is part of Sigil.
**
**  Sigil is free software: you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation, either version 3 of the License, or
**  (at your option) any later version.
**
**  Sigil is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**  GNU General Public License for more details.
**
**  You should have received a copy of the GNU General Public License
**  along with Sigil.  If not, see <http://www.gnu.org/licenses/>.
**
*************************************************************************/

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtGui/QDesktopServices>
#include <QtGui/QKeyEvent>
#include <QtGui/QMessageBox>
#include <QtGui/QTextDocument>
#include <QtWebKit/QWebFrame>

#include "BookManipulation/Book.h"
#include "Misc/SettingsStore.h"
#include "Misc/Utility.h"
#include "PCRE/PCRECache.h"
#include "sigil_constants.h"
#include "ViewEditors/BookViewEditor.h"

const int PROGRESS_BAR_MINIMUM_DURATION = 1500;
const QString BV_BREAK_TAG_INSERT = "<hr class=\"sigilBVTmpChapterBreak\" />";
const QString BV_BREAK_TAG_SEARCH  = "(<div>\\s*)?<hr\\s*class\\s*=\\s*\"[^\"]*sigilBVTmpChapterBreak\[^\"]*\"\\s*/>(\\s*</div>)?";

const QString BREAK_TAG_INSERT    = "<hr class=\"sigilChapterBreak\" />";
// %1 = CKE path.
// %2 = Text to load.
// %3 = language
// %4 = customConfig : '/custom/ckeditor_config.js'.
const QString CKE_BASE =
    "<html>"
    "<head>"
    "    <script type=\"text/javascript\" src=\"%1/ckeditor.js\"></script>"
    "</head>"
    "<body>"
    "    <textarea id=\"editor\" name=\"editor\"></textarea>"
    "    <script type=\"text/javascript\">"
    "        CKEDITOR.replace('editor', { fullPage: true, startupFocus: true, extraPlugins: 'onchange,docprops', language: '%2',"
    "            coreStyles_bold: { element : 'span', attributes :  {'style': 'font-weight: bold;'} },"
    "            coreStyles_italic: { element : 'span', attributes : {'style': 'font-style: italic;'}},"
    "            coreStyles_underline: { element : 'span', attributes : {'style': 'text-decoration: underline;'}},"
    "            coreStyles_strike: { element : 'span', attributes : {'style': 'text-decoration: line-through;'}, onchangeverrides : 'strike' },"
    "            coreStyles_subscript : { element : 'span', attributes : {'style': 'vertical-align: sub; font-size: smaller;'}, overrides : 'sub' },"
    "            coreStyles_superscript : { elementent : 'span', attributes : {'style': 'vertical-align: super; font-size: smaller;'}, overrides : 'sup' },"
    "        %3});"
    "        CKEDITOR.on('instanceReady', function(e) { e.editor.execCommand('maximize'); });"
    "        CKEDITOR.instances.editor.on('change', function() { BookViewEditor.TextChangedFilter(); });"
    "        CKEDITOR.editor.prototype.sigil_apply_selection = function(sel) {"
    "            function _set(r, n, off, func1, func2) {"
    "                if (n.nodeType == 3) {"
    "                    if (off > n.nodeValue.length) { off = n.nodeValue.length; }"
    "                    func1.call(r, n, off);"
    "                } else { func2.call(r, n); }"
    "            }"
    "            function _traverse(el, addr) {"
    "                if (addr.length == 0) { return el; }"
    "                var index = addr.shift();"
    "                if (index < el.childNodes.length) { return _traverse(el.childNodes[index], addr); }"
    "                return el.lastChild ? el.lastChild : el;"
    "            }"
    "            var range = this.document.$.createRange();"
    "            var start = _traverse(this.document.$.body, sel.start_address);"
    "            _set(range, start, sel.start_offset, range.setStart, range.setStartAfter);"
    "            if (sel.end_address) {"
    "                var end = _traverse(this.document.$.body, sel.end_address);"
    "                _set(range, end, sel.end_offset, range.setEnd, range.setEndAfter);"
    "            } else { range.collapse(true); }"
    //"            console.log(range.startContainer + ':' + range.startOffset + ' ' + range.endContainer + ':' + range.endOffset);"
    "            this.document.$.getSelection().removeAllRanges();"
    "            this.document.$.getSelection().addRange(range);"
    "            this.getSelection().scrollIntoView();"
    "        }"
    "    </script>"
    "</body>"
    "</html>";

const QString GET_SELECTION =
    "var ranges = CKEDITOR.instances.editor.getSelection().getRanges();"
    "var range = ranges.length > 0 ? ranges[0] : null;"
    "!range ? null :"
    "        '{\"start_address\":[' + range.startContainer.getAddress().slice(1) + '],\"start_offset\":' + range.startOffset +"
    "          ',\"end_address\":' + (range.collapsed ? 'null' : ('[' + range.endContainer.getAddress().slice(1) + ']')) +"
    "           ',\"end_offset\":' + range.endOffset + '}';"
    ;

const QString SET_DATA_AND_SELECTION =
    "function load_new_data(data) {"
    "   try { var set_data = CKEDITOR.instances.editor.setData; }" // on the initial load, CKEDITOR may not have fully loaded yet
    "   catch (ex) { setTimeout(function() { load_new_data(data); }, 25); return; }"
    "   CKEDITOR.instances.editor.setData(data, function() {"
    "       this.focus();"
    "       var saved_selection = %1;"
    "       if (saved_selection) { this.sigil_apply_selection(saved_selection); }"
    "   });"
    "}"
    "var new_data = BookViewEditor.new_data;"
    "load_new_data(new_data);";

BookViewEditor::BookViewEditor(QWidget *parent)
    :
    BookViewPreview(parent)
{
    page()->settings()->setAttribute(QWebSettings::DeveloperExtrasEnabled, false);
    page()->settings()->setAttribute(QWebSettings::JavascriptCanAccessClipboard, true);
}

void BookViewEditor::CustomSetDocument(const QString &path, const QString &html)
{
    m_isLoadFinished = false;
    m_path = path;

    QString cke_path;
#ifdef Q_WS_MAC
    cke_path = QCoreApplication::applicationDirPath() + "/../ckeditor";
#else
    cke_path = QCoreApplication::applicationDirPath() + "/ckeditor";
#endif
// On *nix we have an installation path we need to check too.
#ifdef Q_WS_X11
    if (cke_path.isEmpty() || !QDir(cke_path).exists()) {
        cke_path = QCoreApplication::applicationDirPath() + "/../share/" + QCoreApplication::applicationName().toLower() + "/ckeditor";
    }
#endif

    QString cke_settings;
    QString cke_setting_path = QDesktopServices::storageLocation(QDesktopServices::DataLocation) + "/ckeditor_config.js";
    if (QFile::exists(cke_setting_path)) {
        cke_settings = QString("customConfig: '%1'").arg(cke_setting_path);
    }

    SettingsStore settings;
    QString base = CKE_BASE.arg(QDir::fromNativeSeparators(cke_path)).arg(settings.uiLanguage()).arg(cke_settings);
    setHtml(base, QUrl::fromLocalFile(path));
    page()->mainFrame()->addToJavaScriptWindowObject("BookViewEditor", this);
    CustomUpdateDocument(html, false);
}

void BookViewEditor::CustomUpdateDocument(const QString &html, bool saveSelection)
{
    m_isLoadFinished = false;

    QString selection = "null";
    if (saveSelection)
    {
        const QVariant result = EvaluateJavascript(GET_SELECTION);
        if (result.type() == QVariant::String)
        {
            selection = result.toString();
        }
    }

    setProperty("new_data", html);
    EvaluateJavascript(SET_DATA_AND_SELECTION.arg(selection));
    // clean-up: by the time EvaluateJavascript returns, the value has been at least copied internally, even if not yet applied
    setProperty("new_data", QVariant());
}

void BookViewEditor::ScrollToFragment(const QString &fragment)
{
    if (fragment.isEmpty()) {
        ScrollToTop();
        return;
    }

    QString scroll = "var documentWrapper = CKEDITOR.instances.editor.dom.document;"
        "var element = documentWrapper.getById(\"" % fragment % "\");"
        "element.scrollIntoView()";
    EvaluateJavascript(scroll % SET_CURSOR_JS);
}

void BookViewEditor::ScrollToFragmentAfterLoad(const QString &fragment)
{
    if (fragment.isEmpty()) {
        return;
    }

    QString scroll = "var documentWrapper = CKEDITOR.instances.editor.dom.document;"
        "var element = documentWrapper.getById(\"" % fragment % "\");"
        "element.scrollIntoView()";
    QString javascript = "window.addEventListener('load', GoToFragment, false);"
        "function GoToFragment() { " % scroll % SET_CURSOR_JS % "}";

    EvaluateJavascript(javascript);
}

QString BookViewEditor::GetHtml()
{
    QString command = "CKEDITOR.instances.editor.getData();";
    return EvaluateJavascript(command).toString();
}

#if 0
QString BookViewEditor::GetXHtml11()
{
    return GetHtml();
}

QString BookViewEditor::GetHtml5()
{
    return GetHtml();
}
#endif

void BookViewEditor::InsertHtml(const QString &html)
{
    QString javascript = "CKEDITOR.instances.editor.insertHtml('" + html + "');";
    EvaluateJavascript(javascript);
}

QString BookViewEditor::SplitChapter()
{
    // Add a temporary break marker so we know where the user has requested
    // the text to be split.
    InsertHtml(BV_BREAK_TAG_INSERT);

    QString new_text;
    QString text = GetHtml();

    QRegExp body_search(BODY_START);
    int body_tag_start = text.indexOf(body_search);

    QString head = text.left(body_tag_start);

    QRegExp break_tag(BV_BREAK_TAG_SEARCH);
    int break_index = text.indexOf(break_tag, body_tag_start + body_search.matchedLength());
    if (break_index != -1) {
        // Create a new HTML string with the text that is being split.
        new_text = Utility::Substring(0, break_index, text) + "</body></html>";

        // Remove the the split text from this document and set it
        // as the text in the editor.
        text = head + "<body>" + Utility::Substring(break_index + break_tag.matchedLength(), text.length(), text);
        CustomSetDocument(m_path, text);
    }

    return new_text;
}

bool BookViewEditor::IsModified()
{
    QString javascript = "CKEDITOR.instances.editor.checkDirty();";
    return EvaluateJavascript(javascript).toBool();
}

void BookViewEditor::ResetModified()
{
    QString javascript = "CKEDITOR.instances.editor.resetDirty();";
    EvaluateJavascript(javascript);
}

void BookViewEditor::Undo()
{
    QString javascript = "CKEDITOR.instances.editor.execCommand('undo');";
    EvaluateJavascript(javascript);
}

void BookViewEditor::Redo()
{
    QString javascript = "CKEDITOR.instances.editor.execCommand('redo');";
    EvaluateJavascript(javascript);
}

// Overridden so we can emit the FocusLost() signal.
void BookViewEditor::focusOutEvent(QFocusEvent *event)
{
    emit FocusLost(this);
    QWebView::focusOutEvent(event);
}


QString BookViewEditor::GetSelectedText()
{
    QString javascript = "CKEDITOR.instances.editor.getSelection().getSelectedText();";
    return EvaluateJavascript(javascript).toString();
}

bool BookViewEditor::event(QEvent *e)
{
    if (e->type() == QEvent::KeyPress) {
        QKeyEvent *k = static_cast<QKeyEvent *>(e);
        if (k->key() == Qt::Key_Escape) {
            return true;
        }
    }
    return BookViewPreview::event(e);
}

void BookViewEditor::TextChangedFilter()
{
    emit textChanged();
}
