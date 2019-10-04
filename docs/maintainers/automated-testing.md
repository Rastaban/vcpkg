# Automated testing

## PR test steps

Automated PR tests have the following steps:
(*Although each of these steps don't necessarily map to a single job/task in Azure DevOps*)

### 1. **Merge to latest baseline**

Each PR is merged to the latest commit from master.
If the PR has merge conflicts with the baseline then the test will fail.
Although uncommon, it is possible for your PR test to have failures you are unable to reproduce locally if it is not compatible with other more recent commits to master.

### 2. **Build vcpkg**

Runs the appropriate bootstrap-vcpkg script for the architecture.  Fails if vcpkg tools do not build.

### 3. **Calculate which ports are affected by the PR changes**

`vcpkg ci` creates a list of ports that need to be built by calculating the abi tag for each port and checking if the tag is not in the current set of cached build results.  The cached build results include both passing and failing builds from **all** CI pipelines. If the abi tag is missing from the cache it is added to the build plan (unless it is on the "skip" list or has a dependency that is either skipped or known to fail).  If the abi tag is found in the cache then its pass/fail status is added to the results list without adding it to the build plan.

The abi tag for a port is the combined hash of the following:
  + The hash of all files under the ports/_portname_ directory
  + The hash of the current triplet
  + the api tag of all port dependencies from the CONTROL file

Any changes that affect the abi tag hash of a port should cause a rebuild of that port, along with all ports that depend on it.  Changes to other files (such as from the `scripts/cmake`) directory that have the potential to affect the build results are not currently part of the abi tag, so changes to those files need to have a manually queued full-rebuild test run.


### 4. **Calculated build plan is executed (ports are built)**

All ports on the build plan are installed.  If a dependency of a port is in the cached build results then it is uncompressed and installed without building.  The results of each successful port build is cached with all files the port installs.  The result of each failed build is given a tombstone in the build results archive that contains the logs.


### 5. **Analyze port build results against baseline expectations**

The results of the port build phase are compared with the expected results from the ci.baseline.txt file.  Any differences from the baseline cause the pipeline to fail.  The logs for all failing ports are collected from the archived tombstones and attached to the Azure DevOps pipeline.  If there was no abi change from previous runs then the failure logs from the last actual build attempt will be attached.  To force a rebuild of the port in later pipeline runs, ask a member of the vcpkg team to remove the tombstone or make a change to one of the files in the port.

If the pipeline is failing because of a fix to a port build then the failure line must be removed from the baseline file.  Flaky ports can be marked as 'skip' to avoid building or 'ignore' to build in the system but ignore the results.  In general ports that cause conflicts with other ports should be skpped and ports that are flaky for other reasons should be ignored until their issue can be addressed.

### Limitations of the CI system

There are a few limitations of the CI system that require additional testing to PRs.

The CI system installs only the default features of a Port (unless another port has an explicit dependency on non-default features).

Some ports are disabled in the CI tests (see the 'skip' tag from `ci.baseine.txt`, changes to these ports require manual testing.

Missing dependances external to vcpkg can cause failures.  We encourage adding dependences as additional ports in vcpkg when it makes sense, but sometimes this is not resonable.  If you need an additional third party package installed let us know and we may be willing to add it to the CI VM setup, especially if it is available via apt on Unbuntu or homebrew on Mac.

## Links to Azure DevOps pipelines

### PR pipelines

+ [https://dev.azure.com/vcpkg/public/_build](https://dev.azure.com/vcpkg/public/_build)
