.PHONY: test vita usb clean

test:
	pytest -q src/desktop/tests tests
	./tests/run_c_tests.sh

vita:
	$(MAKE) -C src/vita

usb:
	cmake -S src/usb -B build/usb -DVCM_ACTIVE_MTP=ON
	cmake --build build/usb -j12

clean:
	$(MAKE) -C src/vita clean
	cmake --build build/usb --target clean
