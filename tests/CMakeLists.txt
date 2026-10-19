add_executable(pluginlifecycletest pluginlifecycletest.cpp fakes/plugincontexts.h)
target_link_libraries(pluginlifecycletest PRIVATE Fooyin::Gui Qt6::Widgets Qt6::Sql Qt6::Test)
target_compile_definitions(pluginlifecycletest PRIVATE QT_NO_KEYWORDS)
add_dependencies(pluginlifecycletest Headphones)
add_test(NAME headphones.plugin-lifecycle
    COMMAND pluginlifecycletest "$<TARGET_FILE:Headphones>")
set_tests_properties(headphones.plugin-lifecycle PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(previewenginetest previewenginetest.cpp fakes/previewfakes.h)
target_link_libraries(previewenginetest PRIVATE HeadphonesPreviewEngine Qt6::Test)
add_test(NAME headphones.preview-engine COMMAND previewenginetest)
set_tests_properties(headphones.preview-engine PROPERTIES TIMEOUT 45)

add_executable(previewsourcetest previewsourcetest.cpp fakes/previewfakes.h)
target_link_libraries(previewsourcetest PRIVATE HeadphonesPreviewEngine Qt6::Test)
add_test(NAME headphones.preview-source COMMAND previewsourcetest)
set_tests_properties(headphones.preview-source PROPERTIES TIMEOUT 30)

add_executable(previewconvertertest previewconvertertest.cpp)
target_link_libraries(previewconvertertest PRIVATE HeadphonesPreviewEngine Qt6::Test)
add_test(NAME headphones.preview-converter COMMAND previewconvertertest)
set_tests_properties(headphones.preview-converter PROPERTIES TIMEOUT 30)

add_executable(previewoutputtest previewoutputtest.cpp fakes/audiosinkfakes.h fakes/previewfakes.h)
target_link_libraries(previewoutputtest PRIVATE HeadphonesPreviewEngine Qt6::Test)
add_test(NAME headphones.preview-output COMMAND previewoutputtest)
set_tests_properties(headphones.preview-output PROPERTIES TIMEOUT 30)

# Deliberately not registered with CTest: a real sink requires an explicit ID.
add_executable(previewaudiosmoke previewaudiosmoke.cpp)
target_link_libraries(previewaudiosmoke PRIVATE HeadphonesPreviewEngine)

add_executable(headphonesguitest headphonesguitest.cpp fakes/audiosinkfakes.h fakes/previewfakes.h)
target_link_libraries(headphonesguitest PRIVATE HeadphonesGui Qt6::Test)
add_test(NAME headphones.gui COMMAND headphonesguitest)
set_tests_properties(headphones.gui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)

# Loaded into an actual matching fooyin, not a second preview process. It is
# opt-in, requires explicit device IDs/fixtures, and is never installed.
add_library(HeadphonesInProcessDriver MODULE inprocessdriver.cpp inprocessdriver.json)
set_target_properties(HeadphonesInProcessDriver PROPERTIES PREFIX "" OUTPUT_NAME fytest_headphones_inprocess
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/inprocess"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/inprocess")
target_link_libraries(HeadphonesInProcessDriver PRIVATE Fooyin::Gui Qt6::Multimedia Qt6::Test)
target_compile_definitions(HeadphonesInProcessDriver PRIVATE QT_NO_KEYWORDS)
