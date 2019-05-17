# Automated testing

## PR tests

Automated PR tests have the following steps:

1. **Merge to latest baseline**

Each PR is merged to the latest commit from master that has completed the post-checkin CI tests.  This is done so the results of the CI tests can be used as a baseline of what ports are expected to build.  If the PR has merge conflicts with the baseline then the test will fail. Although uncommon, it is possible for your PR test to have failures you are unable to reproduce locally if it is not compatible with other more recent changes to master.

2. **Build vcpkg**

Runs the appropriate bootstrap-vcpkg script.

3. **Calculate which ports are affected by the changes**

vcpkg calculates the abi tag for each port

4. **Build all ports affected by the changes**
5. **Analyze port build results against baseline expectations**


-------------------------------------------
documentation TODO:
+ PR test step details
+ how hashing decides what runs
+ what ports are disabled
+ what causes flaky ports
+ how to investigate failures
+ How the baseline works
+ interaction with Vcpkg-PR-Eager
+ future roadmap
+ CI (post checkin) test documentation
+ tips on investigating failures
+ Add links to the azure devops pipeline
