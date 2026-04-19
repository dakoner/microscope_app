#ifndef PYTHONSCINTILLAEDITOR_H
#define PYTHONSCINTILLAEDITOR_H

#include <Qsci/qsciscintilla.h>
#include <Qsci/qscilexerpython.h>
#include <QStringList>

/**
 * @brief A Python REPL editor using QScintilla with syntax highlighting
 * 
 * Features:
 * - Full Python syntax highlighting
 * - Code folding and line numbers
 * - Command history with up/down navigation
 * - Multi-line input support
 * - Auto-indentation
 * - REPL-style prompt (>>> and ...)
 */
class PythonScintillaEditor : public QsciScintilla
{
    Q_OBJECT

public:
    explicit PythonScintillaEditor(QWidget *parent = nullptr);

    /**
     * @brief Append output text to the editor
     */
    void appendOutput(const QString &text);

    /**
     * @brief Get the current command being typed
     */
    QString getCurrentCommand() const;

    /**
     * @brief Show a new prompt
     */
    void showPrompt();

    /**
     * @brief Reset the editor (clear and show fresh prompt)
     */
    void resetEditor();

    /**
     * @brief Check if we're in multi-line continuation mode
     */
    bool isAwaitingContinuation() const { return m_awaitingContinuation; }

    /**
     * @brief Set multi-line continuation status
     */
    void setAwaitingContinuation(bool awaiting) { m_awaitingContinuation = awaiting; }

signals:
    /**
     * @brief Emitted when user presses Enter with a complete command
     */
    void commandReady(const QString &command);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void onMarginClicked(int margin, int line, Qt::KeyboardModifiers state);

private:
    void setupEditor();
    void handleEnterKey();
    void handleBackspace();
    void navigateHistory(int direction);
    void moveCursorToInputStart();
    void moveCursorToInputEnd();
    QString extractCurrentLineInput();
    bool isCompleteCommand(const QString &code);
    void insertPrompt(bool isContinuation = false);

    QsciLexerPython *m_lexer = nullptr;
    QStringList m_history;
    int m_historyIndex = -1;
    QString m_multilineBuffer;
    bool m_awaitingContinuation = false;
    int m_promptLine = -1;

    static constexpr const char *kPrompt = ">>> ";
    static constexpr const char *kContinuation = "... ";
};

#endif // PYTHONSCINTILLAEDITOR_H
