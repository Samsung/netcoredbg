module FSharpCeBreakpointRepro

open Expecto

// --- Regular function (breakpoints work in both CLI and DAP) ---

let regularFunction () =
    let x = 42                  // Line 8:  SET BREAKPOINT HERE (regular)
    let y = x + 1               // Line 9
    printfn "Regular: x=%d y=%d" x y  // Line 10
    y

// --- Expecto CE test block (breakpoints work in CLI, fail in DAP) ---

let ceTests =
    test "ce breakpoint test" {
        let a = 100              // Line 17: SET BREAKPOINT HERE (CE)
        let b = a + 1            // Line 18
        printfn "CE: a=%d b=%d" a b   // Line 19
        Expect.equal b 101 "b should be 101"  // Line 20
    }

// --- Entry point ---

[<EntryPoint>]
let main argv =
    // Call the regular function so its breakpoint is reachable
    let result = regularFunction ()  // Line 28
    printfn "regularFunction returned %d" result

    // Run the Expecto test
    runTestsWithCLIArgs [] argv ceTests
