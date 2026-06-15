add_executable(basic_signal basic_signal.cpp)
target_link_libraries(basic_signal PRIVATE qfcoro Qt5::Core)

add_executable(future_demo future_demo.cpp)
target_link_libraries(future_demo PRIVATE qfcoro Qt5::Core Qt5::Concurrent)
