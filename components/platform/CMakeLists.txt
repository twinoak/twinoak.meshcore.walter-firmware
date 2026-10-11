# platform/ - the ESP-IDF implementations of interfaces/ (the architecture's hal/): thin, no logic, "kept so thin that
# reading it is the test" (docs/ARCHITECTURE.md §1). Nothing yet: task 03 brings the pins.
idf_component_register(REQUIRES interfaces)
