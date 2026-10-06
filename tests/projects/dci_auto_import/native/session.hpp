#pragma once

// The API declares its own defaults. The Vyx build reads this header directly.
class NativeSession {
public:
    explicit NativeSession(int initial = 11);
    ~NativeSession();
    int add(int amount, int multiplier = 2);
    int read() const;
private:
    int value_;
};
