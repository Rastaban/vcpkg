# Automated testing

## PR test steps

Automated PR tests have the following steps:
(*Although each of these steps don't necessarily map to a single job/task in Azure DevOps*)

### 1. **Merge to latest baseline**

Each PR is merged to the latest commit from master that has completed the post-merge CI tests.
If the PR has merge conflicts with the baseline then the test will fail.
Although uncommon, it is possible for your PR test to have failures you are unable to reproduce locally if it is not compatible with other more recent commits to master.

### 2. **Build vcpkg**

Runs the appropriate bootstrap-vcpkg script for the architecture.  Fails if vcpkg tools do not build.

### 3. **Calculate which ports are affected by the PR changes**

`vcpkg ci` creates a list of ports that need to be built by calculating the abi tag for each port and checking if the tag is not in the current set of cached build results.  The cached build results include both passed and failed builds from **all** CI tests. If the abi tag is missing from the cache it is added to the build plan (unless it is on the "skip" list or has a dependency that is either skipped or known to fail).  If the abi tag is found in the cache then its pass/fail status is added to the results list without adding it to the build plan.

The abi tag for a port is the combined hash of the following:
  + The hash of all files under the ports/_portname_ directory
  + The hash of the current triplet
  + the api tag of all port dependencies from the CONTROL file

Any changes that affect the abi tag hash of a port should cause a rebuild of that port, along with all ports that depend on it.  Changes to other files (such as from the `scripts/cmake`) directory that have the potential to affect the build results are not currently part of the abi tag, so changes to those files need to have a manually queued full-rebuild test run.


### 4. **Calculated build plan is executed (ports are built)**

All ports on the build plan are installed.  If a dependency of a port is in the cached build results then it is uncompressed and installed without building.  The results of each successful port build is cached with all files the port installs.  The result of each failed build is given a tombstone in the build results archive that contains the logs.


### 5. **Analyze port build results against baseline expectations**

The results of the port build phase are compared with the results from the previous steps.  Only port build failures that are not also in the baseline are considered a regression and fail the build.  The logs for all failing ports are collected from the archived tombstones and attached to the Azure DevOps pipeline.  

Because the results comparison also uses the known results from step 3 the failure logs from previous iterations of the PR tests will continue to be attached to the pipeline even if the port was not actually built in the current run.  (It won't be built if the abi tag did not change).  To force a rebuild of the port in later pipeline runs, ask a member of the vcpkg team to remove the tombstone or make a change to one of the files in the port.

## Skipped ports

The automated test system has a list of ports that are not tested.  Ports are added to the exclusion list under the following circumstances:

+ The port build will nondeterministicly fail
+ The port publishes files that conflict with another port

In the case of conflicting ports we take into account popularity, number of dependants, and the chance of regression in the selection of which one is added to the skip list.

## Links to Azure DevOps pipelines

### PR pipelines

+ [vcpkg-linux-PR](https://dev.azure.com/vcpkg/public/_build?definitionId=8)
+ [vcpkg-windows-PR](https://dev.azure.com/vcpkg/public/_build?definitionId=10)
+ [vcpkg-osx-PR](https://dev.azure.com/vcpkg/public/_build?definitionId=12)

### Post merge pipelines

+ [vcpkg-linux-master-CI](https://dev.azure.com/vcpkg/public/_build?definitionId=6)
+ [vcpkg-windows-master-CI](https://dev.azure.com/vcpkg/public/_build?definitionId=9)
+ [vcpkg-osx-master-CI](https://dev.azure.com/vcpkg/public/_build?definitionId=11)

-------------------------------------------
documentation TODO:
+ what ports are disabled/skipped
+ what causes flaky ports
+ explain lack of feature testing
+ how to resolve failures due to dependencies external to vcpkg
