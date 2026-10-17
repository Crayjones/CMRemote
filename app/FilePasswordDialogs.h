#pragma once
#include "Secret.h"
#include <functional>
#include <optional>
class QWidget;
class QDialog;
namespace FilePasswordDialogs {
enum class Mode { Unlock, ChangeMaster };
using PassphraseCompletion = std::function<void(std::optional<Secret>)>;
QDialog* passphrase(QWidget* parent, Mode mode, PassphraseCompletion complete);
}
