add_executable(pluginlifecycletest pluginlifecycletest.cpp fakes/plugincontexts.h)
target_link_libraries(pluginlifecycletest PRIVATE Fooyin::Gui Qt6::Widgets Qt6::Sql Qt6::Test)
target_compile_definitions(pluginlifecycletest PRIVATE QT_NO_KEYWORDS)
add_dependencies(pluginlifecycletest Headphones)
add_test(NAME headphones.plugin-lifecycle
    COMMAND pluginlifecycletest "$<TARGET_FILE:Headphones>")
set_tests_properties(headphones.plugin-lifecycle PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
