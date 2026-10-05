#!/usr/bin/env python3
"""Export the pinned Potion static model to the checked native TLSTAT01 format.

Input is an already downloaded Hugging Face directory. No network access. The
header records the upstream revision and source hashes; SHA256 protects the
whole vocabulary/float payload. Only the supported BERT WordPiece recipe is
accepted. Dimensional truncation is explicit and receives a separate emb_gen.
"""
import argparse
import hashlib
import json
import pathlib
import struct

import numpy as np
from safetensors.numpy import load_file

REVISION = "6fc8051fab2a1e0ee76689cf08c853792ac285e7"
WEIGHTS_SHA256 = "07609e5bd33aad37900b3fd62f4ec96f6daec88ca4d46b9d8b928bfababf6ea0"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--dimensions", type=int, choices=(128, 256, 512), default=256)
    args = parser.parse_args()
    source = pathlib.Path(args.model)
    tokenizer_bytes = (source / "tokenizer.json").read_bytes()
    tokenizer = json.loads(tokenizer_bytes)
    assert tokenizer["normalizer"] == {"type": "BertNormalizer", "clean_text": True, "handle_chinese_chars": True, "strip_accents": None, "lowercase": True}
    assert tokenizer["pre_tokenizer"] == {"type": "BertPreTokenizer"}
    recipe = tokenizer["model"]
    assert recipe["type"] == "WordPiece" and recipe["continuing_subword_prefix"] == "##"
    assert recipe["unk_token"] == "[UNK]" and recipe["max_input_chars_per_word"] == 100
    tensors = load_file(source / "model.safetensors")
    assert set(tensors) == {"embeddings"}, "weighted/remapped models require another backend"
    values = tensors["embeddings"]
    assert values.shape == (len(recipe["vocab"]), 512) and np.isfinite(values).all()
    vocabulary = [None] * len(recipe["vocab"])
    for word, index in recipe["vocab"].items():
        vocabulary[index] = word.encode() + b"\0"
    assert vocabulary[:5] == [x.encode() + b"\0" for x in ("[PAD]", "[UNK]", "[CLS]", "[SEP]", "[MASK]")]
    offsets, offset = [], 0
    for word in vocabulary:
        offsets.append(offset)
        offset += len(word)
    offsets.append(offset)
    body = struct.pack(f"<{len(offsets)}I", *offsets) + b"".join(vocabulary)
    body += values[:, :args.dimensions].astype("<f4").tobytes()
    weights_hash = hashlib.sha256((source / "model.safetensors").read_bytes()).hexdigest()
    assert weights_hash == WEIGHTS_SHA256, "source weights do not match the pinned revision"
    header = b"TLSTAT01" + struct.pack("<III", len(vocabulary), args.dimensions, offset)
    header += REVISION.encode() + weights_hash.encode() + hashlib.sha256(tokenizer_bytes).hexdigest().encode()
    header += hashlib.sha256(body).digest()
    output = pathlib.Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(header + body)
    print(json.dumps({"path": str(output), "bytes": output.stat().st_size, "dimensions": args.dimensions, "payload_sha256": hashlib.sha256(body).hexdigest()}))


if __name__ == "__main__":
    main()
