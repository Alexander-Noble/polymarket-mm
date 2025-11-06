#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

int main() {
    json j = {{"test", "hello world"}};
    std::cout << "JSON: " << j.dump(2) << std::endl;
    std::cout << "C++ setup working!" << std::endl;
    return 0;
}