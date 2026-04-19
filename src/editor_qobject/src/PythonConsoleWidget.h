#ifndef PYTHONCONSOLEWIDGET_H
#define PYTHONCONSOLEWIDGET_H

#include <QPlainTextEdit>
#include <QStringList>
#include <QTimer>

/**
 * @brief A Python REPL-like console widget
 * 
 * Features:
 * - Inline command entry with >>> prompt
 * - Command history with up/down arrow navigation
 * - Multi-line input support (auto-detect continuation)
 * - Readline-like key bindings (Ctrl+A, Ctrl+E, Ctrl+K, etc.)
 * - Output mixed with input display
 * - Separate visual styling for prompts, input, and output
 */
class PythonConsoleWidget : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit PythonConsoleWidget(QWidget *parent = nullptr);

    /**
     * @brief Append output text to console (e.g., from Python execution)
     */
    void appendOutput(const QString &text);

    /**
     * @brief Get the current command being typed (without prompt)
     */
    QString getCurrentCommand() const;

    /**
     * @brief Clear the command input area
     */
    void clearCurrentCommand();

    /**
     * @brief Insert text at the current input position
     */
    void insertAtCurrentInput(const QString &text);

    /**
     * @brief Prompt user for input (show >>> and move cursor to input area)
     */
    void showPrompt();

    /**
     * @brief Check if we're waiting for multi-line continuation (e.g., inside function def)
     */
    bool isAwaitingContinuation() const { return m_awaitingContinuation; }

    /**
     * @brief Set multi-line continuation status
     */
    void setAwaitingContinuation(bool awaiting) { m_awaitingContinuation = awaiting; }

    /**
     * @brief Clear history and console (reset state)
     */
    void resetConsole();

signals:
    /**
     * @brief Emitted when user presses Enter/Return with a complete command
     */
    void returnPressed();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private slots:
    void onCursorPositionChanged();

private:
    void ensurePromptAtEnd();
    void handleReturnKey();
    void handleBackspace();
    void navigateHistory(int direction);  // direction: -1 for up, +1 for down
    void moveCursorToInputStart();
    void moveCursorToInputEnd();
    QString extractCommand();
    void insertPrompt();
    bool isInInputArea() const;
    void applyConsoleFormatting();

    // State tracking
    QStringList m_commandHistory;
    int m_historyIndex = -1;
    QString m_currentInput;
    QString m_multilineBuffer;
    bool m_awaitingContinuation = false;
    int m_promptBlockNumber = -1;
    bool m_blockCursorPositionUpdate = false;

    // UI configuration
    static constexpr const char *kPrompt = ">>> ";
    static constexpr const char *kContinuation = "... ";
};

#endif // PYTHONCONSOLEWIDGET_H
