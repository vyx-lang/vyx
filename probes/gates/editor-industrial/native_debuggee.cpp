#include <cstdio>
#include <map>
#include <string>
#include <vector>

struct Record {
    unsigned id;
    std::string name;
    std::vector<unsigned> samples;
};

// A real native STL workload, with deep frames and large synthetic children.
// The adapter must expand pages on demand rather than materialize this graph.
int inspect(std::vector<Record>& records, std::map<unsigned, std::string>& index, int depth) {
    if (depth) return inspect(records, index, depth - 1);
    unsigned sum = 0;
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        volatile unsigned current = records[iteration].id; // DAP_BREAKPOINT
        sum += current;
    }
    return sum == 4950 && index.size() == records.size() ? 0 : 1;
}

int main() {
    std::vector<Record> records;
    std::map<unsigned, std::string> index;
    records.reserve(100000);
    for (unsigned id = 0; id < 100000; ++id) {
        auto name = std::string("item-") + std::to_string(id);
        records.push_back(Record{id, name, {id, id + 1, id + 2, id + 3}});
        index.emplace(id, name);
    }
    const int result = inspect(records, index, 32);
    // Enough output to expose adapters which keep an unbounded transcript.
    const std::string line(1023, 'x');
    for (int i = 0; i < 4096; ++i) std::puts(line.c_str());
    std::fflush(stdout);
    return result;
}
