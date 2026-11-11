# The dashboard: one REST contract (api/), served by server/, and the page in web/, with a chart.js widget, in a browser.

# The contract, which the server and the page include.
add_library(quoll_dashboard_api INTERFACE)
target_include_directories(quoll_dashboard_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/api)
target_link_libraries(quoll_dashboard_api INTERFACE quoll::rest)

# The components of the page, headers but for the chart. The test of the page runs them natively, in place of the calls of
# web/products_client.cpp, which go through fetch.
add_library(quoll_dashboard_web_components INTERFACE)
# The chart's view, compiled in each program using the components: the widget's JSON stays out of the page's sources.
target_sources(quoll_dashboard_web_components INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/web/components/sales_chart.cpp)
target_include_directories(quoll_dashboard_web_components INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/web)
target_link_libraries(quoll_dashboard_web_components INTERFACE quoll_dashboard_api quoll::ui)

# The handlers, which their test calls through a REST client, as a page would.
add_library(quoll_dashboard_service INTERFACE)
target_include_directories(quoll_dashboard_service INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(quoll_dashboard_service INTERFACE quoll_dashboard_api)

quoll_declare_tests(SOURCES tests/dashboard_page_test.cpp tests/service_test.cpp DEPS quoll_dashboard_web_components
                    quoll_dashboard_service)

if(EMSCRIPTEN)
  add_subdirectory(web)
  return()
endif()

add_executable(quoll_dashboard_server server/main.cpp)
target_link_libraries(quoll_dashboard_server PRIVATE quoll_dashboard_service quoll::runtime quoll::server)

# What dangles is an error in a sample too.
if(TARGET quoll_warnings)
  target_link_libraries(quoll_dashboard_server PRIVATE quoll_warnings)
endif()
