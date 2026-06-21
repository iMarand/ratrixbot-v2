#pragma once

// Shared by every strategy module (RSI-threshold, confluence, any future one)
// so they can all be swapped in/out of the backtester and paper trader
// through the same interface.
enum class Signal { None, Rise, Fall };