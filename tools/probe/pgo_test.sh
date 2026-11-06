#!/bin/bash
set -e
src="$(dirname "$0")/probe.cpp"
rm -rf /tmp/pgodata && mkdir -p /tmp/pgodata
g++ -O3 -fPIC -fprofile-generate -fprofile-dir=/tmp/pgodata "$src" -shared -o /tmp/probe_pg.so
cat > /tmp/runner.cpp <<'CPP'
#include <cstdint>
extern "C" uint64_t probe_sum(const uint64_t*, size_t);
int main(){ uint64_t a[1024]={1}; return (int)probe_sum(a,1024); }
CPP
g++ -O3 -fPIC -fprofile-generate -fprofile-dir=/tmp/pgodata "$src" /tmp/runner.cpp -o /tmp/probe_pg
/tmp/probe_pg || true
g++ -O3 -fPIC -fprofile-use -fprofile-dir=/tmp/pgodata -c "$src" -o /tmp/probe_use.o && echo "PGO use OK"
