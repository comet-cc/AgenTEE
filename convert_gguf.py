import argparse
import os
import subprocess
from huggingface_hub import login, snapshot_download

def main():
    p = argparse.ArgumentParser()
    p.add_argument("repo_name", help="HF repo like TinyLlama/TinyLlama-1.1B-Chat-v1.0")
    p.add_argument("hf_token", help="HF token")
    p.add_argument("--llama_cpp_dir", default="../llamacpp", help="Path to llama.cpp")
    p.add_argument("--outtype", default="q4_k_m", help="Quant type: f16, q4_k_m, q5_0, q8_0, ...")
    args = p.parse_args()

    # 1) Auth
    login(token=args.hf_token, add_to_git_credential=True)

    # 2) Download repo locally (Transformers format)
    local_dir = snapshot_download(repo_id=args.repo_name, token=args.hf_token)
    repo_dir_name = args.repo_name.split("/")[-1]

    # 3) Convert with llama.cpp script
    converter = os.path.join(args.llama_cpp_dir, "convert_hf_to_gguf.py")
    script_dir = os.path.dirname(os.path.abspath(__file__))
    tmp_dir = os.path.abspath(os.path.join(script_dir, "..", "tmp"))
    os.makedirs(tmp_dir, exist_ok=True)
    outfile = os.path.join(tmp_dir, f"{repo_dir_name}-{args.outtype}.gguf")

    cmd = [
        "python3", converter,
        "--outtype", args.outtype,
        "--outfile", outfile,
        "--use-temp-file",
        local_dir
    ]
    print("Running:", " ".join(cmd))
    res = subprocess.run(cmd, text=True, capture_output=True)
    if res.returncode != 0:
        print(res.stderr)
        raise SystemExit("Conversion failed")
    print(res.stdout)
    print(f"Done: {outfile}")

if __name__ == "__main__":
    main()
