# Helpers of the final-run scripts (macOS): socsens (SoC temperatures), fsbench (file-system fsync test).
all: socsens fsbench

socsens: socsens.c
	xcrun clang -O2 -o $@ $< -framework IOKit -framework CoreFoundation

fsbench: fsbench.c
	xcrun clang -O2 -o $@ $<

clean:
	rm -f socsens fsbench

.PHONY: all clean
