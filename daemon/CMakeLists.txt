add_executable(socrates-daemon main.cpp)
target_link_libraries(socrates-daemon PRIVATE socrates)
set_project_warnings(socrates-daemon)
