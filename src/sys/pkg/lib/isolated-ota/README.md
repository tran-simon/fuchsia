isolated-ota
============

The `isolated-ota` library provides a simple interface that allows a Fuchsia
system to be installed over the air to a given blobfs and paver from a provided
TUF repository and channel.

It does this by setting up the software delivery stack:
1. `pkg-cache` is launched against the provided blobfs.
2. `pkg-resolver` is launched, using the provided repository configuration and
   channel, along with `pkg-cache` from step 2.
3. If Omaha configuration is provided (an Omaha app id, and a URL to use for the
   Omaha server), the `omaha-client` state machine is launched. It performs an
   update check once, and the Omaha state machine calls the system-updater with
   the update package URI returned by Omaha.
4. If no Omaha configuration is provided, `isolated-ota` launches the system
   updater directly, using the default update URL.
5. The system updater runs an OTA, resolving all of the packages in the new
   system using the `pkg-resolver` from step 3, and paving the images in the
   update package using the provided paver.

To use this library:
*  you need to make sure your image includes the package
   `//src/sys/pkg/lib/isolated-swd:isolated-swd-components`,
* your calling component needs to be CFv2, and you must list the manifest at
`//src/sys/pkg/meta/pkg-recovery.cml` as a child of your calling component, and
* you need to route all the capabilities that `pkg-recovery.cm` requires.