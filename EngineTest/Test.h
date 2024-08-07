// Copyright (c) Contributors of Primal+
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
#pragma once
#include <thread>
#include <chrono>
#include <string>

// What test are we performing?
#define TEST_ENTITY_COMPONENTS 0
#define TEST_WINDOW 0
#define TEST_RENDERER 1

class Test
{
public:
#ifdef _WIN64
    virtual bool initialize() = 0;
    virtual void run() = 0;
#elif __linux__
    virtual bool initialize(void* disp) = 0;
    virtual void run(void* disp) = 0;
#endif
    virtual void shutdown() = 0;
};

#ifdef _WIN64
#include <Windows.h>
#else
#include <iostream>
#endif // _WIN64

class time_it
{
public:
    using clock = std::chrono::steady_clock;
    using time_stamp = std::chrono::steady_clock::time_point;

    // average frame time per seconds
    constexpr float dt_avg() const { return _dt_avg * 1e-3f; }

    void begin()
    {
        _start = clock::now();
    }

    void end()
    {
        auto dt = clock::now() - _start;
        _us_avg += ((float)std::chrono::duration_cast<std::chrono::milliseconds>(dt).count() - _us_avg) / (float)_counter;
        ++_counter;
        _dt_avg = _us_avg;

        if (std::chrono::duration_cast<std::chrono::seconds>(clock::now() - _seconds).count() >= 1)
        {
#ifdef _WIN64
            OutputDebugStringA("Avg. frame (ms): ");
            OutputDebugStringA(std::to_string(_us_avg * 0.001f).c_str());
            OutputDebugStringA((" " + std::to_string(_counter)).c_str());
            OutputDebugStringA(" fps");
            OutputDebugStringA("\n");
#else
            std::cout << "Avg. frame (ms): ";
            std::cout << std::to_string(_ms_avg).c_str();
            std::cout << (" " + std::to_string(_counter)).c_str();
            std::cout << " fps" << std::endl;
#endif // _WIN64
            _us_avg = 0.0f;
            _counter = 1;
            _seconds = clock::now();
        }
    }
private:
    float           _dt_avg{ 16.667f };
    float		    _us_avg{ 0.0f };
    int			    _counter{ 1 };
    time_stamp	    _start;
    time_stamp	    _seconds{ clock::now() };
};