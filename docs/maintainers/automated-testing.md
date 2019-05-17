# Automated testing

## PR tests

PR tests have the following steps:
1. Merge to latst baseline
2. Build vcpkg
3. Calculate which ports are affected by the changes
4. Build all ports affected by the changes
5. Analyze port build results against baseline expectations


-------------------------------------------
documentation TODO:
+ PR test step details
+ how hashing decides what runs
+ what ports are disabled
+ what causes flaky ports
+ how to investigate failures
+ How the baseline works
