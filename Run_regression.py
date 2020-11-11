#!/usr/bin/python3

# Copyright (c) 2018-2019 Bluespec, Inc.
# See LICENSE for license details

import sys
import os
import stat
import subprocess
import argparse

import multiprocessing

# ================================================================
# DEBUGGING ONLY: This exclude list allows skipping some specific test

exclude_list = []

n_workers_max = 4

# ================================================================

def main (argv = None):
    # Command-line parsing
    parser = argparse.ArgumentParser (
              formatter_class=argparse.RawDescriptionHelpFormatter
            , description='Runs a regression of the ISA tests'
            , epilog = """
                The following run-time directories and logs are created:
                    isa-logs/fail: Contains simulation logs of all failing tests 
                    isa-logs/pass: Contains simulation logs of all passing tests 
                    worker*: temporary simulation directories for each worker process\n"""
            )
    parser.add_argument (
              '--simulator'
            , action="store"
            , dest='sim_path'
            , help='The simulator executable'
            , default="simulator.exe"
            )

    parser.add_argument (
              '--tools'
            , action="store"
            , dest='tool_path'
            , help='Path to tools like elf_to_hex'
            , default="."
            )

    parser.add_argument (
              '--tests'
            , action="store"
            , dest='test_path'
            , help="Path to the ISA tests elf files"
            , default="."
            )

    parser.add_argument (
              '--logs'
            , action="store"
            , dest='logs_path'
            , help="Where should the logs be dumped?"
            , default="."
            )

    parser.add_argument (
              '--arch'
            , action="store"
            , dest='arch_string'
            , help="RV architecture string and implied test families"
            , default="rv32i"
            )

    parser.add_argument (
              '--verbosity'
            , type=int
            , action="store"
            , dest='verbosity'
            , help="""Verbosity of simulation output
                      (0: quiet, 1: instruction trace, 2: detailed output)"""
            , default=0
            )

    parser.add_argument (
              '--num-jobs'
            , type=int
            , action="store"
            , dest='workers'
            , help="Number of concurrent workers"
            , default=int (multiprocessing.cpu_count () // 3)
            )
    args = parser.parse_args()

    # Simulation executable
    args_dict = {'sim_path' : os.path.abspath (os.path.normpath (args.sim_path))}
    args_dict ['tool_path'] = os.path.abspath (os.path.normpath (args.tool_path))
    args_dict ['elfs_path'] = os.path.abspath (os.path.normpath (args.test_path))
    args_dict ['logs_path'] = os.path.abspath (os.path.normpath (args.logs_path))
    pass_path = os.path.join (args_dict['logs_path'], 'pass')
    fail_path = os.path.join (args_dict['logs_path'], 'fail')
    for path in (args_dict['logs_path'], pass_path, fail_path) :
        if not os.path.isdir (path) :
            print ("Creating dir: " + path)
            os.mkdir (path)

    # Architecture string and implied ISA test families
    args_dict ['arch_string'] = extract_arch_string (args.arch_string)
    test_families = select_test_families (args_dict['arch_string'])
    print ("Testing the following families of ISA tests")
    for tf in test_families:
        print ("    " + tf)
    args_dict ['test_families'] = test_families
    args_dict ['verbosity'] = args.verbosity

    n_workers = args.workers
    sys.stdout.write ("Using {0} worker processes\n".format (n_workers))

    # End of command-line arg processing
    # ================================================================

    # elf_to_hex executable
    elf_to_hex_exe = os.path.join (args_dict['tool_path'], "elf_to_hex")
    if (not os.path.exists (elf_to_hex_exe)):
        sys.stderr.write ("ERROR: elf_to_hex executable does not exist?\n")
        sys.stderr.write ("    at {0}\n".format (elf_to_hex_exe))
        sys.stdout.write ("\n")
        return 1
    args_dict ['elf_to_hex_exe'] = elf_to_hex_exe

    sys.stdout.write ("Parameters:\n")
    for key in iter (args_dict):
        sys.stdout.write ("    {0:<16}: {1}\n".format (key, args_dict [key]))

    def fn_filter_regular_file (level, filename):
        (dirname, basename) = os.path.split (filename)
        # Ignore filename if has any extension (heuristic that it's not an ELF file)
        if "." in basename: return False

        # TEMPORARY FILTER WHILE DEBUGGING:
        if basename in exclude_list:
            sys.stdout.write ("WARNING: TEMPORARY FILTER IN EFFECT; REMOVE AFTER DEBUGGING\n")
            sys.stdout.write ("    This test is in exclude_list: {0}\n".format (basename))
            return False

        # Ignore filename if does not match test_families
        for x in args_dict ['test_families']:
            if basename.find (x) != -1: return True
        return False

    def fn_filter_dir (level, filename):
        return True

    # Traverse the elfs_path and collect filenames of relevant isa tests
    filenames = traverse (fn_filter_dir, fn_filter_regular_file, 0, args_dict['elfs_path'])
    n_tests   = len (filenames)

    # Print the filenames of the elf tests
    try : test_list = open ('isa-tests.lst', 'w')
    except : 
        logging.error ("Unable to open file 'isa-tests.lst' for writing")
        sys.exit (1)

    for f in filenames : test_list.write ('%s\n' % f)
    test_list.close ()

    sys.stdout.write ("{0} relevant isa tests found under {1}\n".format (n_tests, args_dict['elfs_path']))
    if n_tests == 0: return 0

    args_dict ['filenames'] = filenames
    args_dict ['n_tests']   = n_tests

    # Create a shared counter to index into the list of filenames
    index = multiprocessing.Value ('L', 0)    # Unsigned long (4 bytes)
    args_dict ['index'] = index

    # Create a shared array for each worker's (n_executed, n_passed) results
    results = multiprocessing.Array ('L', [ 0 for j in range (2 * n_workers) ])
    args_dict ['results'] = results

    # Create n workers
    sys.stdout.write ("Creating {0} workers (sub-processes)\n".format (n_workers))
    workers        = [multiprocessing.Process (target = do_worker,
                                               args = (w, args_dict))
                      for w in range (n_workers)]

    # Start the workers
    for worker in workers: worker.start ()

    # Wait for all workers to finish
    for worker in workers: worker.join ()

    # Collect all results
    num_executed = 0
    num_passed   = 0
    with results.get_lock ():
        for w in range (n_workers):
            n_e = results [2 * w]
            n_p = results [2 * w + 1]
            sys.stdout.write ("Worker {0} executed {1} tests, of which {2} passed\n"
                              .format (w, n_e, n_p))
            num_executed = num_executed + n_e
            num_passed   = num_passed   + n_p

    # Write final statistics
    sys.stdout.write ("Total tests: {0} tests\n".format (n_tests))
    sys.stdout.write ("Executed:    {0} tests\n".format (num_executed))
    sys.stdout.write ("PASS:        {0} tests\n".format (num_passed))
    sys.stdout.write ("FAIL:        {0} tests\n".format (num_executed - num_passed))

    # Set nonzero exit code unless all tests passed
    return 0

# ================================================================
# Extract the architecture string (e.g., RV64AIMSU) from the string s

def extract_arch_string (s):
    s1     = s.upper()
    j_rv32 = s1.find ("RV32")
    j_rv64 = s1.find ("RV64")

    if (j_rv32 >= 0):
        j = j_rv32
    elif (j_rv64 >= 0):
        j = j_rv64
    else:
        sys.stderr.write ("ERROR: cannot find architecture string beginning with RV32 or RV64 in: \n")
        sys.stderr.write ("    '" + s + "'\n")
        sys.exit (1)

    k = j + 4
    rv = s1 [j:k]

    extns = ""
    while (k < len (s)):
        ch = s [k]
        if (ch < "A") or (ch > "Z"): break
        extns = extns + s [k]
        k     = k + 1

    arch = rv + extns
    return arch

# ================================================================
# Select ISA test families based on provided arch string

def select_test_families (arch):
    arch = arch.lower ()
    families = []

    if arch.find ("32") != -1:
        rv = 32
        families = ["rv32ui-p", "rv32mi-p"]
    else:
        rv = 64
        families = ["rv64ui-p", "rv64mi-p"]

    if (arch.find ("s") != -1):
        s = True
        if rv == 32:
            families.extend (["rv32ui-v", "rv32si-p"])
        else:
            families.extend (["rv64ui-v", "rv64si-p"])
    else:
        s = False

    def add_family (extension):
        if (arch.find (extension) != -1):
            if rv == 32:
                families.append ("rv32u" + extension + "-p")
                if s:
                    families.append ("rv32u" + extension + "-v")
            else:
                families.append ("rv64u" + extension + "-p")
                if s:
                    families.append ("rv64u" + extension + "-v")

    add_family ("m")
    add_family ("a")
    add_family ("f")
    add_family ("d")
    add_family ("c")

    return families

# ================================================================
# Recursively traverse the dir tree below path and collect filenames
# that pass the given filter functions

def traverse (fn_filter_dir, fn_filter_regular_file, level, path):
    st = os.stat (path)
    is_dir = stat.S_ISDIR (st.st_mode)
    is_regular = stat.S_ISREG (st.st_mode)

    if is_dir and fn_filter_dir (level, path):
        files = []
        for entry in os.listdir (path):
            path1 = os.path.join (path, entry)
            files.extend (traverse (fn_filter_dir, fn_filter_regular_file, level + 1, path1))
        return files

    elif is_regular and fn_filter_regular_file (level, path):
        return [path]

    else:
        return []

# ================================================================
# For each ELF file, execute it in the RISC-V simulator

def do_worker (worker_num, args_dict):
    tmpdir = "./worker_" + "{0}".format (worker_num)
    if not os.path.exists (tmpdir):
        os.mkdir (tmpdir)
    elif not os.path.isdir (tmpdir):
        sys.stdout.write ("ERROR: Worker {0}: {1} exists but is not a dir".format (worker_num, tmpdir))
        return

    os.chdir (tmpdir)
    sys.stdout.write ("Worker {0} using dir: {1}\n".format (worker_num, tmpdir))

    n_tests   = args_dict ['n_tests']
    filenames = args_dict ['filenames']
    index     = args_dict ['index']
    results   = args_dict ['results']

    num_executed = 0
    num_passed   = 0

    while True:
        # Get a unique index into the filenames, and get the filename
        with index.get_lock():
            my_index    = index.value
            index.value = my_index + 1
        if my_index >= n_tests:
            # All done
            with results.get_lock():
                results [2 * worker_num]     = num_executed
                results [2 * worker_num + 1] = num_passed
            return
        filename = filenames [my_index]

        (message, passed) = do_isa_test (worker_num, args_dict, filename)
        num_executed = num_executed + 1

        if passed:
            num_passed = num_passed + 1
            pass_fail = "PASS"
        else:
            pass_fail = "FAIL"

        message = message + ("Worker {0}: Test: {1} {2} [So far: total {3}, executed {4}, PASS {5}, FAIL {6}]\n"
                             .format (worker_num,
                                      os.path.basename (filename),
                                      pass_fail,
                                      n_tests,
                                      num_executed,
                                      num_passed,
                                      num_executed - num_passed))
        sys.stdout.write (message)

# ================================================================
# For each ELF file, execute it in the RISC-V simulator

def do_isa_test (worker_no, args_dict, full_filename):
    message = ""

    (dirname, basename) = os.path.split (full_filename)

    # Construct the commands for sub-process execution
    command1 = [args_dict ['elf_to_hex_exe'], full_filename, "Mem.hex"]

    command2 = [args_dict ['sim_path'], "+tohost", "+jtag_port=666{0}".format(worker_no)]
    command2.append ("+vpi_port=777{0}".format(worker_no))
    if (args_dict ['verbosity'] == 1): command2.append ("+v1")
    elif (args_dict ['verbosity'] == 2): command2.append ("+v2")

    message = message + "    Exec:"
    for x in command1:
        message = message + (" {0}".format (x))
    message = message + "\n"

    message = message + ("    Exec:")
    for x in command2:
        message = message + (" {0}".format (x))
    message = message + ("\n")

    # Run command as a sub-process
    completed_process1 = run_command (command1)
    completed_process2 = run_command (command2)
    passed = completed_process2.stdout.find ("PASS") != -1

    # Save stdouts in log file
    log_filename = os.path.join (
        args_dict ['logs_path'], "pass" if passed else "fail", basename + ".log")
    message = message + ("    Writing log: {0}\n".format (log_filename))

    fd = open (log_filename, 'w')
    fd.write (completed_process1.stdout)
    fd.write (completed_process2.stdout)
    fd.close ()

    # If Tandem Verification trace file was created, save it as well
    if os.path.exists ("./trace_out.dat"):
        trace_filename = log_filename.rsplit('.', 1)[0] + ".trace_data"
        os.rename ("./trace_out.dat", trace_filename)
        message = message + ("    Trace output saved in: {0}\n".format (trace_filename))

    return (message, passed)

# ================================================================
# This is a wrapper around 'subprocess.run' because of an annoying
# incompatible change in moving from Python 3.5 to 3.6

def run_command (command):
    python_minor_version = sys.version_info [1]
    if python_minor_version < 6:
        # Python 3.5 and earlier
        result = subprocess.run (args = command,
                                 timeout = 30,
                                 bufsize = 0,
                                 stdout = subprocess.PIPE,
                                 stderr = subprocess.STDOUT,
                                 universal_newlines = True)
    else:
        # Python 3.6 and later
        result = subprocess.run (args = command,
                                 timeout = 30,
                                 bufsize = 0,
                                 stdout = subprocess.PIPE,
                                 stderr = subprocess.STDOUT,
                                 encoding='utf-8')
    return result

# ================================================================
# For non-interactive invocations, call main() and use its return value
# as the exit code.
if __name__ == '__main__':
  sys.exit (main (sys.argv))
