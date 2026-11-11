fn main() {
    cxx_build::bridges(["src/bridge.rs", "src/ssh.rs"]).include("include").std("c++17").compile("remotemanager-cxx");
    println!("cargo:rerun-if-changed=include/SshEventSink.h");
    println!("cargo:rerun-if-changed=src/bridge.rs");
    println!("cargo:rerun-if-changed=src/ssh.rs");
}
