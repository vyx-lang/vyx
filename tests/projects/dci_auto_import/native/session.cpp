#include "session.hpp"
NativeSession::NativeSession(int initial) : value_(initial) {}
NativeSession::~NativeSession() { value_ = 0; }
int NativeSession::add(int amount, int multiplier) { return value_ += amount * multiplier; }
int NativeSession::read() const { return value_; }
