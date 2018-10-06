#pragma once
#include <CoreMinimal.h>
#include <Editor.h>
#include <Editor/TransBuffer.h>

#include <functional>

/**
 * Utility functions.
 */
class sfUtils
{
public:
    typedef std::function<void()> Callback;

    /**
     * Calls a delegate then clears any undo transactions that were added during the delegate execution.
     *
     * @param   Callback callback
     */
    static void PreserveUndoStack(Callback callback)
    {
        UTransBuffer* undoBufferPtr = Cast<UTransBuffer>(GEditor->Trans);
        int undoCount = 0;
        int undoNum = 0;
        if (undoBufferPtr != nullptr)
        {
            undoCount = undoBufferPtr->UndoCount;
            undoBufferPtr->UndoCount = 0;
            undoNum = undoBufferPtr->UndoBuffer.Num();
        }
        callback();
        if (undoBufferPtr != nullptr)
        {
            while (undoBufferPtr->UndoBuffer.Num() > undoNum)
            {
                undoBufferPtr->UndoBuffer.Pop();
            }
            undoBufferPtr->UndoCount = undoCount;
        }
    }

    /**
     * Converts FString to std::string.
     *
     * @param   FString inString
     * @return  std::string
     */
    static std::string FToStdString(FString inString)
    {
        return std::string(TCHAR_TO_UTF8(*inString));
    }
};
