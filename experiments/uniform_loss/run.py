import os
import subprocess
import sys
import time
import multiprocessing
import numpy
from datetime import datetime


def wait_for_finished_run():
    subprocess.run(["pos", "commands", "await", str(id_sender)], stdout=subprocess.DEVNULL,
                   stderr=subprocess.STDOUT)
    subprocess.run(["pos", "commands", "await", str(id_receiver)], stdout=subprocess.DEVNULL,
                   stderr=subprocess.STDOUT)


code_rate = sys.argv[1]
max_loss = int(sys.argv[2])
runs_per_loss = int(sys.argv[3])
window_size = sys.argv[4]
total_runs = 10 * runs_per_loss * max_loss
failed_runs = 0

sender = open("run_sender_" + code_rate + ".sh", "w")
receiver = open("run_receiver.sh", "w")
sender.write("#!/bin/bash\n\nclient/udpSender -s 10.0.0.1 -r 10.0.0.4 -c " + code_rate + " -ws " + window_size)
sender.close()
receiver.write("#!/bin/bash\n\nclient/udpReceiver -s 10.0.0.1 -r 10.0.0.4")
receiver.close()

print("Starting with measurements... Total runs: " + str(total_runs))
starttime = time.time()
counter = 0
for i in numpy.arange(0, max_loss, 0.1):
    print("Packet loss of: %.1f" % i)
    os.system("pos commands launch litecoincash tc qdisc change dev eno7 root netem loss " + str(i) + "%")
    os.system("pos commands launch litecoincash tc qdisc change dev eno8 root netem loss " + str(i) + "%")
    for j in range(0, runs_per_loss):
        id_sender = subprocess.check_output(
            ["pos", "commands", "launch", "--infile", "run_sender_" + str(code_rate) + ".sh", "litecoin",
             "--non-blocking"]).decode(sys.stdout.encoding).strip()
        time.sleep(0.5)
        id_receiver = subprocess.check_output(
            ["pos", "commands", "launch", "--infile", "run_receiver.sh", "litecoingold", "--non-blocking"]).decode(
            sys.stdout.encoding).strip()

        wait = multiprocessing.Process(target=wait_for_finished_run, name="wait")
        wait.start()
        wait.join(180)

        if wait.is_alive():
            print("run failed: timeout")
            os.system("pos commands launch litecoin pkill 'udpSender'")
            os.system("pos commands launch litecoingold pkill 'udpReceiver'")
            wait.join()
            failed_runs += 1

        counter += 1
        elapsed = time.time() - starttime
        estimated = (elapsed / counter) * total_runs
        finish = starttime + estimated
        finish = datetime.fromtimestamp(finish).strftime("%H:%M:%S")
        left = estimated - elapsed

        print("Current status: " + str(counter) + "/" + str(total_runs) + " (%.2f" % (
                counter / total_runs * 100) + "%) finished, " + str(
            failed_runs) + " failed runs, estimated time remaining for this run: %.2f" % left + "s (%.2f" % (
                      left / 60) + "min), ETC: " + finish)

current_time = datetime.now().timestamp()
date_time = datetime.fromtimestamp(current_time)
str_date_time = date_time.strftime("%d_%m_%Y_%H_%M")

os.system("scp litecoingold:logs/measurementsReceiver_" + str(
    code_rate) + ".csv ~/results/" + str_date_time + "_measurementsReceiver_" + str(code_rate) + ".csv")
os.system("scp litecoin:logs/measurementsSender_" + str(
    code_rate) + ".csv ~/results/" + str_date_time + "_measurementsSender_" + str(code_rate) + ".csv")
print("finished")
