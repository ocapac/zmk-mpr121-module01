if(CONFIG_MPR121_MODULE01)
	zephyr_library()

	zephyr_library_sources(
		src/disc_mode_behavior.c
		src/mpr121_disc_kscan.c
	)

	zephyr_library_include_directories(include)
endif()
