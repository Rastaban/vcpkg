# Automated testing

## PR tests

Automated PR tests have the following steps:

### 1. **Merge to latest baseline**

Each PR is merged to the latest commit from master that has completed the post-checkin CI tests.  This is done so the results of the CI tests can be used as a baseline of what ports are expected to build.  If the PR has merge conflicts with the baseline then the test will fail. Although uncommon, it is possible for your PR test to have failures you are unable to reproduce locally if it is not compatible with other more recent changes to master.

### 2. **Build vcpkg**

Runs the appropriate bootstrap-vcpkg script.

### 3. **Calculate which ports are affected by the changes**

`vcpkg ci` creates the list of ports that need testing by calculating the abi tag for each port and comparing the tag with the current set of cached build results.  The cached build results include both passed and failed results builds from all CI tests. If the abi tag is missing from the cache it is added to the build list (unless it is on the "skip" list or has a dependancy that is either skipped or known to fail)

The abi tag is the combined hash of the following:
  + The hash of all files under the ports/_portname_ directory
  + The hash of the current triplet
  + the api tag of all port dependancies from the CONTROL file

### 4. **Build all ports affected by the changes**



### 5. **Analyze port build results against baseline expectations**

And upload relevent logs (even if the failure did not happen in this build)

-------------------------------------------
documentation TODO:
+ PR test step details
+ what ports are disabled/skipped
+ what causes flaky ports
+ how to investigate failures
+ How the baseline works
+ interaction with Vcpkg-PR-Eager
+ future roadmap
+ CI (post checkin) test documentation
+ tips on investigating failures
+ Add links to the azure devops pipeline
+ explain lack of feature testing
+ mention the cache results are added to each run and the side effects of this
+ how to resolve failures due to dependancies external to vcpkg
