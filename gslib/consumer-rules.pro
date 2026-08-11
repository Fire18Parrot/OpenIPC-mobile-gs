# The JNI bridge calls these by name through GetMethodID, so R8 must not rename
# or remove them however the consuming app is configured.
-keep class org.openipc.gslib.NativeGroundStation { *; }
-keep class org.openipc.gslib.NativeGroundStation$Bridge { *; }
-keep interface org.openipc.gslib.GroundStationListener { *; }
