if( EOSIO_COMPILE_TEST_CONTRACTS )
   add_contract_with_abi( restrict_action_test restrict_action_test restrict_action_test.cpp )
   target_include_directories( restrict_action_test PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} )
   install( TARGETS restrict_action_test DESTINATION ${CMAKE_CURRENT_SOURCE_DIR} )
else()
   configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/restrict_action_test.wasm ${CMAKE_CURRENT_BINARY_DIR}/restrict_action_test.wasm COPYONLY )
   configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/restrict_action_test.abi  ${CMAKE_CURRENT_BINARY_DIR}/restrict_action_test.abi  COPYONLY )
endif()
