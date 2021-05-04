contract C {
    event Test(function() external indexed);
    function f() public {
        emit Test(this.f);
    }
}
// ====
// compileViaYul: also
// ----
// f() ->
// ~ emit: #0x16e407cc31a66b716200af7eadb4155035b14f295af52acbc4356b464d649b71, #0x0fdd67305928fcac8d213d1e47bfa6165cd0b87b26121ff00000000000000000
