# Native rebuild and CTest recheck — 2026-09-22

Environment: Ubuntu 24.04 under WSL2, x86_64 host.

Commands:

```sh
cmake --build /tmp/mqmgateway-release-build --target tests mqmgateway_iot -j2
ctest --test-dir /tmp/mqmgateway-release-build --output-on-failure
```

Result: PASS. Both build targets completed successfully. CTest reported 2/2 tests
passed; `unit_tests` completed in 170.36 s. The rebuilt test binary includes the
five `pipeline_metrics_tests` cases (29 assertions).

This is a native regression check only; it is not an ARM64 hardware result.
