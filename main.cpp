/**
 * @file main.cpp
 * @author modelTR (mxtian_ran@163.com)
 * @brief Main file
 * @version 0.1
 * @date 2026-11-02
 * @copyright MIT License (c) 2026
 * 
 */

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <thread>

#include "concurrent_stack.hpp"

constexpr size_t CAPACITY = 10;

auto thread_a(concur::Stack<int, CAPACITY> &stack) -> void{
    if(stack.push(2)){
        std::cout << "thread a push 2" << '\n';
    }
    std::cout << "thread a pop" << stack.pop() << '\n';
}

auto thread_b(concur::Stack<int, CAPACITY> &stack) -> void{
    std::cout << "thread b pop" << stack.pop() << '\n';
    if(stack.push(5)){
        std::cout << "thread b push 5" << '\n';
    }
}


auto main() -> int {

    try {
    concur::Stack<int, CAPACITY> stack{1, 2 ,3};

    std::jthread a{thread_a, std::ref(stack)};
    std::jthread b{thread_b, std::ref(stack)};

    a.join();
    b.join();

    stack.print();

    return EXIT_SUCCESS;
    } catch (std::exception &e) {
        std::cout << e.what();
    }

    
}