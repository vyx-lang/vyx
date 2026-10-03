extern "C" int duplicate_a();
extern "C" int duplicate_b();

int main() {
    return duplicate_a() == 41 && duplicate_b() == 42 ? 0 : 1;
}
