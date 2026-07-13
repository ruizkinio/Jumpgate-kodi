"""Open the native Jumpgate profile manager."""

import xbmc


if __name__ == "__main__":
    # The addon has no privileged command channel. Native foreground UI owns
    # every profile mutation and confirmation.
    xbmc.executebuiltin("JumpgateManager", True)
