MESON_FLAGS :=

ifeq ($(RELEASE), 1)
	MESON_FLAGS += --buildtype=release
else
	MESON_FLAGS += --buildtype=debug -Db_sanitize=undefined,address \
				   -Db_coverage=true
endif

all: build/
	meson compile -C build/

build/:
	meson setup $(MESON_FLAGS) build/

reset:
	rm -rf build/

clean:
	meson compile -C build/ --clean

run: all
	@GSETTINGS_SCHEMA_DIR=schemas/ LSAN_OPTIONS=fast_unwind_on_malloc=0 \
						 build/clippor --debug --server

test: all
	meson test --print-errorlogs -C build/

.PHONY: all reset clean run test
