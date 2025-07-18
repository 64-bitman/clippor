MESON_FLAGS :=

ifeq ($(RELEASE), 1)
	MESON_FLAGS += --buildtype=release
else
	MESON_FLAGS += --buildtype=debug -Db_sanitize=address,undefined
endif
ifeq ($(COVERAGE), 1)
	MESON_FLAGS += -Db_coverage=true
endif

all: build/
	meson compile -C build/

build/:
	meson setup $(MESON_FLAGS) build/

reset:
	rm -rf build/

clean:
	meson compile -C build/ --clean
	lcov --directory . --zerocounters
	rm -rf build/out build/coverage.info build/coverage.filtered.info

run: all
	@GSETTINGS_SCHEMA_DIR=schemas/ LSAN_OPTIONS=fast_unwind_on_malloc=0 \
						 build/clippor --debug --server

test: all
	meson test --verbose -C build/

coverage:
	lcov --capture --directory . --output-file build/coverage.info --rc lcov_branch_coverage=1
	lcov --remove build/coverage.info '/usr/*' 'tests/*' 'tomlc17.*' \
		--output-file build/coverage.filtered.info --rc lcov_branch_coverage=1
	genhtml build/coverage.filtered.info --branch-coverage --output-directory build/coverage

show_coverage: coverage
	xdg-open build/coverage/index.html

.PHONY: all reset clean run test coverage show_coverage
