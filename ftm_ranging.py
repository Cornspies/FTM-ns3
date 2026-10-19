import subprocess
import time
import numpy as np

results_path = "ftm_ranging/"
error_models = ["wired/", "wireless/", "sig_str/", "sig_str_fading/"]

def performTests():
    error_model_index = 0
    for curr_error_model in error_models:
        for curr_dist in range(1, 101):
            print(curr_error_model, curr_dist, "m")
            output = results_path + curr_error_model
            if curr_dist < 10:
                output += "0" + str(curr_dist)
            else:
                output += str(curr_dist)
            output += "m"

            # create empty output file for ns-3 to append
            curr_file = open(output, "w")
            curr_file.close()

            if curr_dist % 5 != 0:
                continue
            
            ns3_command = ["ftm-ranging",
                    "--distance=" + str(curr_dist),
                    "--error=" + str(error_model_index),
                    "--filename=" + output
                    ]
            ns3_string = ' '.join(ns3_command)
            subprocess.run(["./ns3", "run", ns3_string])
        error_model_index += 1

def main():
    performTests()

if __name__ == "__main__":
    start_time = time.perf_counter()
    main()
    end_time = time.perf_counter()
    time_elapsed = round(end_time - start_time, 3)
    print("DONE")
    print("Time elapsed: " + str(time_elapsed) + 's')

