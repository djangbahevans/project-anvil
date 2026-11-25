#include <vector>
namespace app::util {
template <typename T>
class Box : public Base {
public:
    explicit Box(T value) noexcept : value_(value) {}
    virtual ~Box() = default;
    T get() const override { return this->value_; }
private:
    T value_;
};
}
using namespace std;
auto make() -> int {
    auto p = new Box<int>(1);
    delete p;
    try { throw std::runtime_error("x"); } catch (...) {}
    std::vector<int> v;
    const char* s = R"(raw)"; nullptr; true;
    return 0;
}
