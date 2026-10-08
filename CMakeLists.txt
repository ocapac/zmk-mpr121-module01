if(CONFIG_MPR121_MODULE01)
	zephyr_library()

	zephyr_library_sources(
		src/disc_mode_behavior.c
		src/mpr121_disc_kscan.c
		src/virtual_input.c
	)

	zephyr_library_include_directories(include)

	#
	# Make ZMK app headers available to this module.
	# This is needed for headers like:
	#   drivers/behavior.h
	#   zmk/behavior.h
	#
	foreach(app_dir ${APPLICATION_SOURCE_DIR} ${PROJECT_SOURCE_DIR} ${CMAKE_SOURCE_DIR})
		if(EXISTS "${app_dir}/include")
			zephyr_library_include_directories("${app_dir}/include")
		endif()
	endforeach()
endif()
