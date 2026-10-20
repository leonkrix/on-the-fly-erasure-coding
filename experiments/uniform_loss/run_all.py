import os
import time
from datetime import datetime

here = os.path.dirname(os.path.abspath(__file__))
results = os.path.expanduser("~/results")

current_time = datetime.now().timestamp()
date_time = datetime.fromtimestamp(current_time)
str_date_time = date_time.strftime("%d_%m_%Y_%H_%M")
os.makedirs(results + "/run_" + str_date_time, exist_ok=True)

for i in range(2, 7):
    for j in range(1, 4):
        print("Starting new run with code rate of: " + str(i) + " and max sliding window size of: " + str(j * 256))
        time.sleep(5)
        os.system("python3 " + os.path.join(here, "run.py") + " " + str(i) + " 20 1 " + str(j * 256))
        os.system("scp litecoingold:logs/measurementsReceiver_" + str(
            i) + ".csv ~/results/run_" + str_date_time + "/measurementsReceiver_" + str(i) + "_" + str(
            j * 256) + ".csv")
        os.system("scp litecoin:logs/measurementsSender_" + str(
            i) + ".csv ~/results/run_" + str_date_time + "/measurementsSender_" + str(i) + "_" + str(
            j * 256) + ".csv")
        os.system("pos commands launch litecoin rm logs/measurementsSender_" + str(i) + ".csv")
        os.system("pos commands launch litecoingold rm logs/measurementsReceiver_" + str(i) + ".csv")
