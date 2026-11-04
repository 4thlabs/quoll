quoll_main_tests()
quoll_compile_fail_tests()

get_property(quoll_ui_hot_test GLOBAL PROPERTY QUOLL_UI_HOT_TEST)

if(quoll_ui_hot_test)
  add_test(NAME ui_hot COMMAND ${quoll_ui_hot_test})
endif()
