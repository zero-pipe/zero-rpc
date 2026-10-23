if(NB_ENABLE_DDS)
    nb_add_example(example_14_dds_pubsub main.c)
    if(WIN32 AND NB_CYCLONEDDS_DLL)
        add_custom_command(TARGET example_14_dds_pubsub POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${NB_CYCLONEDDS_DLL}
                $<TARGET_FILE_DIR:example_14_dds_pubsub>/ddsc.dll)
    endif()
endif()
