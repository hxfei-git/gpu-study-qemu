# SPDX-License-Identifier: Apache-2.0
# Copyright 2024 The Qwen team, Alibaba Group and The HuggingFace Inc. team. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""客体 Qwen 文本分词：只在调用者的 CPU 上读取本地词表、合并规则和聊天模板。

字节映射、相邻符号对和 BPE 合并循环改编自 transformers 4.48.3：
https://github.com/huggingface/transformers/blob/v4.48.3/src/transformers/models/qwen2/tokenization_qwen2.py
本文件保留其版权与 Apache-2.0 授权，完整许可证附在文件末尾。
改动包括独立的配置装载、有界缓存，以及按 tokenizer.json 处理 NFC 和 added tokens；
这不是完整的 Hugging Face tokenizer 包，也不导入 Transformers、Torch 或 Rust 扩展。

依赖只有标准库、regex 和 Jinja2（客体可安装 py3-regex、py3-jinja2）。调用者必须以
python -I 启动；模块不下载材料、不安装依赖、不访问 WSL 服务、不执行模型前向。
支持 Qwen2 的 NFC、既定预切分表达式、无 dropout 的 ByteLevel BPE，以及不裁剪空白、
不参与规范化的 added tokens。配置变化若超出这些语义会明确拒绝，不静默套用近似规则。

QwenTokenizer(material_directory) 提供 encode、decode、apply_chat_template、encode_chat。
encode 不额外添加 BOS/EOS；decode 默认跳过配置中 special=true 的 token，不清理空白，
对不完整 UTF-8 使用 replace。config.json 存在时 vocabulary 是模型的词表维度，允许
尾部未分配的填充 ID；这些 ID 与官方 fast decoder 一样不产生文本。不存在时使用实际
词表大小。eos_ids 来自 generation_config.json；本模块不解释其中的采样参数。

聊天模板直接使用 tokenizer_config.json 的原文，在 Jinja2 不可变沙箱内执行。只接受
非空的单段对话列表，消息仅含 role/content，role 为 system、user 或 assistant，content
为字符串；不支持 tools、tool_calls、tool 角色、多模态消息、批处理或 assistant mask。
不负责截断、padding、上下文容量检查或流式增量解码；生成文本应累计 ID 后整体 decode。
"""

from collections import OrderedDict
import json
from pathlib import Path
import sys
import unicodedata

import regex
from jinja2 import meta
from jinja2.sandbox import ImmutableSandboxedEnvironment


# 保留 transformers 4.48.3 的 Qwen2 预切分语义；配置中必须使用同一表达式。
_PRETOKENIZE_REGEX = (
    r"(?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}|"
    r" ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+"
)
# 每个实例最多缓存的 BPE 片段数；不缓存超过下述字节数的片段，避免长输入滞留。
_CACHE_ENTRIES = 4096
_CACHE_MAX_BYTES = 256


def _require(condition, detail):
    if not condition:
        raise ValueError("不支持或不一致的 Qwen 分词配置：" + detail)


def _read_json(directory, name):
    with (directory / name).open(encoding="utf-8") as stream:
        return json.load(stream)


def _bytes_to_unicode():
    """沿用上游的可逆字节映射，避免 BPE 符号中出现空白或控制字符。"""
    values = (
        list(range(ord("!"), ord("~") + 1))
        + list(range(ord("¡"), ord("¬") + 1))
        + list(range(ord("®"), ord("ÿ") + 1))
    )
    symbols = values[:]
    extra = 0
    for value in range(2**8):
        if value not in values:
            values.append(value)
            symbols.append(2**8 + extra)
            extra += 1
    return dict(zip(values, map(chr, symbols)))


def _get_pairs(word):
    """返回当前符号序列中所有相邻对；符号可能已由多个字节合并而成。"""
    return set(zip(word, word[1:]))


class QwenTokenizer:
    """从本地官方材料构造独立分词器；读取材料前检查调用者启用了 -I。"""

    def __init__(self, material_directory):
        if not sys.flags.isolated:
            raise ValueError("读取 Qwen 材料必须使用 python -I")
        directory = Path(material_directory)
        backend = _read_json(directory, "tokenizer.json")
        settings = _read_json(directory, "tokenizer_config.json")
        self._load_bpe(directory, backend, settings)
        self._load_added_tokens(backend, settings)

        actual_size = len(self._decoder)
        config = (_read_json(directory, "config.json")
                  if (directory / "config.json").is_file() else {})
        _require(config.get("model_type", "qwen2") == "qwen2", "config.json.model_type")
        self.vocabulary = config.get("vocab_size", actual_size)
        _require(type(self.vocabulary) is int and self.vocabulary >= actual_size,
                 "config.json.vocab_size 小于实际词表或不是整数")
        generation = _read_json(directory, "generation_config.json")
        eos = generation.get("eos_token_id")
        self.eos_ids = [eos] if type(eos) is int else eos
        _require(isinstance(self.eos_ids, list) and bool(self.eos_ids)
                 and all(type(value) is int and value in self._special_ids
                         for value in self.eos_ids),
                 "generation_config.json.eos_token_id 必须指向已声明的特殊 token")
        self.eos_ids = list(self.eos_ids)
        self._load_template(settings)
        self._cache = OrderedDict()

    def _load_bpe(self, directory, backend, settings):
        """只装载已支持的确定性管线，并核对 JSON 与独立词表、merge 文件一致。"""
        _require(set(backend) == {
            "version", "truncation", "padding", "added_tokens", "normalizer",
            "pre_tokenizer", "post_processor", "decoder", "model",
        }, "tokenizer.json 的管线字段")
        _require(backend["version"] == "1.0" and backend["truncation"] is None
                 and backend["padding"] is None, "版本、自动截断或 padding")
        _require(backend["normalizer"] == {"type": "NFC"}, "normalizer 必须为 NFC")
        byte_level = {"type": "ByteLevel", "add_prefix_space": False,
                      "trim_offsets": False, "use_regex": False}
        _require(backend["pre_tokenizer"] == {
            "type": "Sequence",
            "pretokenizers": [
                {"type": "Split", "pattern": {"Regex": _PRETOKENIZE_REGEX},
                 "behavior": "Isolated", "invert": False},
                byte_level,
            ],
        }, "pre_tokenizer 必须使用既定 Qwen Split + ByteLevel")
        _require(backend["decoder"] == byte_level
                 and backend["post_processor"] == byte_level,
                 "decoder 或 post_processor")
        model = backend["model"]
        _require({key: value for key, value in model.items()
                  if key not in ("vocab", "merges")} == {
            "type": "BPE", "dropout": None, "unk_token": None,
            "continuing_subword_prefix": "", "end_of_word_suffix": "",
            "fuse_unk": False, "byte_fallback": False,
        }, "BPE 参数")
        supported = {
            "add_bos_token": False, "add_prefix_space": False, "bos_token": None,
            "clean_up_tokenization_spaces": False, "errors": "replace",
            "split_special_tokens": False, "tokenizer_class": "Qwen2Tokenizer",
            "unk_token": None,
        }
        _require(set(settings) <= set(supported) | {
            "added_tokens_decoder", "additional_special_tokens", "chat_template",
            "eos_token", "pad_token", "model_max_length",
        }, "tokenizer_config.json 含未支持字段")
        _require(all(settings.get(key, value) == value for key, value in supported.items()),
                 "tokenizer_config.json 的编码、解码选项")
        self._encoder = model["vocab"]
        _require(self._encoder == _read_json(directory, "vocab.json"),
                 "vocab.json 与 tokenizer.json 不一致")
        _require(all(isinstance(token, str) and type(value) is int
                     for token, value in self._encoder.items())
                 and set(self._encoder.values()) == set(range(len(self._encoder))),
                 "基础词表 ID 必须从 0 连续且唯一")
        self._byte_encoder = _bytes_to_unicode()
        self._byte_decoder = {value: key for key, value in self._byte_encoder.items()}
        _require(all(symbol in self._encoder for symbol in self._byte_decoder)
                 and all(all(char in self._byte_decoder for char in token)
                         for token in self._encoder), "基础词表不是完整的 ByteLevel 词表")

        merges = []
        with (directory / "merges.txt").open(encoding="utf-8") as stream:
            for index, line in enumerate(stream):
                line = line.strip()
                if not line or (index == 0 and line.startswith("#version:")):
                    continue
                merges.append(tuple(line.split()))
        _require([" ".join(pair) for pair in merges] == model["merges"],
                 "merges.txt 与 tokenizer.json 不一致")
        _require(all(len(pair) == 2 and all(part in self._encoder for part in pair)
                     and "".join(pair) in self._encoder for pair in merges)
                 and len(set(merges)) == len(merges), "BPE merge 必须有效且唯一")
        self._bpe_ranks = dict(zip(merges, range(len(merges))))
        self._pattern = regex.compile(_PRETOKENIZE_REGEX)

    def _load_added_tokens(self, backend, settings):
        """在规范化之前识别 added tokens；special 标记只控制解码时是否跳过。"""
        added = backend["added_tokens"]
        configured = settings["added_tokens_decoder"]
        _require(isinstance(added, list) and bool(added), "added_tokens 必须是非空列表")
        _require(all(set(token) == {
            "id", "content", "single_word", "lstrip", "rstrip", "normalized", "special",
        } and type(token["id"]) is int and isinstance(token["content"], str)
            and bool(token["content"]) and type(token["special"]) is bool
            and all(token[key] is False
                    for key in ("single_word", "lstrip", "rstrip", "normalized"))
            for token in added), "added_tokens 的边界、空白或规范化选项")
        _require(configured == {
            str(token["id"]): {key: value for key, value in token.items() if key != "id"}
            for token in added
        }, "added_tokens_decoder 与 tokenizer.json 不一致")
        first_id = len(self._encoder)
        _require(sorted(token["id"] for token in added)
                 == list(range(first_id, first_id + len(added))), "added token ID 必须连续追加")
        self._added = {token["content"]: token["id"] for token in added}
        _require(len(self._added) == len(added)
                 and not self._added.keys() & self._encoder.keys(), "added token 内容重复")
        self._special_ids = {token["id"] for token in added if token["special"]}
        special_contents = {token["content"] for token in added if token["special"]}
        declared = list(settings["additional_special_tokens"])
        declared.extend(settings[key] for key in ("eos_token", "pad_token")
                        if settings.get(key) is not None)
        _require(set(declared) == special_contents, "特殊 token 声明与 special 标记不一致")
        self._decoder = [None] * (first_id + len(added))
        for token, value in self._encoder.items():
            self._decoder[value] = token
        for token, value in self._added.items():
            self._decoder[value] = token
        self._added_pattern = regex.compile("|".join(
            regex.escape(token) for token in sorted(self._added, key=len, reverse=True)))

    def _load_template(self, settings):
        self.chat_template = settings.get("chat_template")
        _require(isinstance(self.chat_template, str) and bool(self.chat_template),
                 "chat_template 必须为官方模板字符串")
        environment = ImmutableSandboxedEnvironment(
            trim_blocks=True, lstrip_blocks=True, extensions=["jinja2.ext.loopcontrols"])
        syntax = environment.parse(self.chat_template)
        _require(meta.find_undeclared_variables(syntax) <= {
            "messages", "tools", "add_generation_prompt",
        }, "chat_template 使用了未支持的变量")
        self._template = environment.from_string(syntax)

    def _bpe(self, token):
        """沿用上游的按 rank 合并循环，改用每实例有界 LRU，返回符号元组。"""
        cached = self._cache.get(token)
        if cached is not None:
            self._cache.move_to_end(token)
            return cached
        word = tuple(token)
        pairs = _get_pairs(word)
        while pairs:
            bigram = min(pairs, key=lambda pair: self._bpe_ranks.get(pair, float("inf")))
            if bigram not in self._bpe_ranks:
                break
            first, second = bigram
            new_word = []
            index = 0
            while index < len(word):
                try:
                    found = word.index(first, index)
                except ValueError:
                    new_word.extend(word[index:])
                    break
                new_word.extend(word[index:found])
                index = found
                if word[index] == first and index < len(word) - 1 and word[index + 1] == second:
                    new_word.append(first + second)
                    index += 2
                else:
                    new_word.append(word[index])
                    index += 1
            word = tuple(new_word)
            pairs = _get_pairs(word)
        if len(token) <= _CACHE_MAX_BYTES:
            self._cache[token] = word
            if len(self._cache) > _CACHE_ENTRIES:
                self._cache.popitem(last=False)
        return word

    def _encode_plain(self, text, result):
        text = unicodedata.normalize("NFC", text)
        for match in self._pattern.finditer(text):
            token = "".join(self._byte_encoder[value] for value in match[0].encode("utf-8"))
            result.extend(self._encoder[part] for part in self._bpe(token))

    def encode(self, text: str) -> list[int]:
        """编码一段 UTF-8 文本；先识别原样的 added tokens，再规范化其余文本。"""
        if not isinstance(text, str):
            raise TypeError("encode 的输入必须是字符串")
        result = []
        start = 0
        for match in self._added_pattern.finditer(text):
            self._encode_plain(text[start:match.start()], result)
            result.append(self._added[match[0]])
            start = match.end()
        self._encode_plain(text[start:], result)
        return result

    def decode(self, ids, skip_special_tokens: bool = True) -> str:
        """先拼接 ByteLevel 字节再解码，保留空白；填充 ID 不产生文本。"""
        if type(skip_special_tokens) is not bool:
            raise TypeError("skip_special_tokens 必须是布尔值")
        raw = bytearray()
        for value in ids:
            if type(value) is not int or not 0 <= value < self.vocabulary:
                raise ValueError("decode 的 token ID 必须是模型词表范围内的整数")
            if value >= len(self._decoder) or (skip_special_tokens and value in self._special_ids):
                continue
            token = self._decoder[value]
            try:
                piece = bytes(self._byte_decoder[char] for char in token)
            except KeyError:
                piece = token.encode("utf-8")
            raw.extend(piece)
        return raw.decode("utf-8", errors="replace")

    def apply_chat_template(self, messages, add_generation_prompt: bool = True) -> str:
        """在沙箱中执行材料原有模板；只支持 role/content 文本消息，不支持 tools。"""
        if not isinstance(messages, (list, tuple)) or not messages:
            raise ValueError("messages 必须是非空的对话列表")
        if type(add_generation_prompt) is not bool:
            raise TypeError("add_generation_prompt 必须是布尔值")
        clean = []
        for message in messages:
            if (not isinstance(message, dict) or set(message) != {"role", "content"}
                    or message["role"] not in ("system", "user", "assistant")
                    or not isinstance(message["content"], str)):
                raise ValueError("只支持 system/user/assistant 的 role/content 文本消息，"
                                 "不支持 tools、tool_calls 或多模态内容")
            clean.append({"role": message["role"], "content": message["content"]})
        return self._template.render(messages=clean, tools=None,
                                     add_generation_prompt=add_generation_prompt)

    def encode_chat(self, messages, add_generation_prompt: bool = True) -> list[int]:
        """先应用官方聊天模板，再编码；不额外插入特殊 token。"""
        return self.encode(self.apply_chat_template(messages, add_generation_prompt))


# 下列为上游 transformers 4.48.3 分发包 LICENSE 的原文，随单文件模块保留。
# Copyright 2018- The Hugging Face team. All rights reserved.
#
#                                  Apache License
#                            Version 2.0, January 2004
#                         http://www.apache.org/licenses/
#
#    TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION
#
#    1. Definitions.
#
#       "License" shall mean the terms and conditions for use, reproduction,
#       and distribution as defined by Sections 1 through 9 of this document.
#
#       "Licensor" shall mean the copyright owner or entity authorized by
#       the copyright owner that is granting the License.
#
#       "Legal Entity" shall mean the union of the acting entity and all
#       other entities that control, are controlled by, or are under common
#       control with that entity. For the purposes of this definition,
#       "control" means (i) the power, direct or indirect, to cause the
#       direction or management of such entity, whether by contract or
#       otherwise, or (ii) ownership of fifty percent (50%) or more of the
#       outstanding shares, or (iii) beneficial ownership of such entity.
#
#       "You" (or "Your") shall mean an individual or Legal Entity
#       exercising permissions granted by this License.
#
#       "Source" form shall mean the preferred form for making modifications,
#       including but not limited to software source code, documentation
#       source, and configuration files.
#
#       "Object" form shall mean any form resulting from mechanical
#       transformation or translation of a Source form, including but
#       not limited to compiled object code, generated documentation,
#       and conversions to other media types.
#
#       "Work" shall mean the work of authorship, whether in Source or
#       Object form, made available under the License, as indicated by a
#       copyright notice that is included in or attached to the work
#       (an example is provided in the Appendix below).
#
#       "Derivative Works" shall mean any work, whether in Source or Object
#       form, that is based on (or derived from) the Work and for which the
#       editorial revisions, annotations, elaborations, or other modifications
#       represent, as a whole, an original work of authorship. For the purposes
#       of this License, Derivative Works shall not include works that remain
#       separable from, or merely link (or bind by name) to the interfaces of,
#       the Work and Derivative Works thereof.
#
#       "Contribution" shall mean any work of authorship, including
#       the original version of the Work and any modifications or additions
#       to that Work or Derivative Works thereof, that is intentionally
#       submitted to Licensor for inclusion in the Work by the copyright owner
#       or by an individual or Legal Entity authorized to submit on behalf of
#       the copyright owner. For the purposes of this definition, "submitted"
#       means any form of electronic, verbal, or written communication sent
#       to the Licensor or its representatives, including but not limited to
#       communication on electronic mailing lists, source code control systems,
#       and issue tracking systems that are managed by, or on behalf of, the
#       Licensor for the purpose of discussing and improving the Work, but
#       excluding communication that is conspicuously marked or otherwise
#       designated in writing by the copyright owner as "Not a Contribution."
#
#       "Contributor" shall mean Licensor and any individual or Legal Entity
#       on behalf of whom a Contribution has been received by Licensor and
#       subsequently incorporated within the Work.
#
#    2. Grant of Copyright License. Subject to the terms and conditions of
#       this License, each Contributor hereby grants to You a perpetual,
#       worldwide, non-exclusive, no-charge, royalty-free, irrevocable
#       copyright license to reproduce, prepare Derivative Works of,
#       publicly display, publicly perform, sublicense, and distribute the
#       Work and such Derivative Works in Source or Object form.
#
#    3. Grant of Patent License. Subject to the terms and conditions of
#       this License, each Contributor hereby grants to You a perpetual,
#       worldwide, non-exclusive, no-charge, royalty-free, irrevocable
#       (except as stated in this section) patent license to make, have made,
#       use, offer to sell, sell, import, and otherwise transfer the Work,
#       where such license applies only to those patent claims licensable
#       by such Contributor that are necessarily infringed by their
#       Contribution(s) alone or by combination of their Contribution(s)
#       with the Work to which such Contribution(s) was submitted. If You
#       institute patent litigation against any entity (including a
#       cross-claim or counterclaim in a lawsuit) alleging that the Work
#       or a Contribution incorporated within the Work constitutes direct
#       or contributory patent infringement, then any patent licenses
#       granted to You under this License for that Work shall terminate
#       as of the date such litigation is filed.
#
#    4. Redistribution. You may reproduce and distribute copies of the
#       Work or Derivative Works thereof in any medium, with or without
#       modifications, and in Source or Object form, provided that You
#       meet the following conditions:
#
#       (a) You must give any other recipients of the Work or
#           Derivative Works a copy of this License; and
#
#       (b) You must cause any modified files to carry prominent notices
#           stating that You changed the files; and
#
#       (c) You must retain, in the Source form of any Derivative Works
#           that You distribute, all copyright, patent, trademark, and
#           attribution notices from the Source form of the Work,
#           excluding those notices that do not pertain to any part of
#           the Derivative Works; and
#
#       (d) If the Work includes a "NOTICE" text file as part of its
#           distribution, then any Derivative Works that You distribute must
#           include a readable copy of the attribution notices contained
#           within such NOTICE file, excluding those notices that do not
#           pertain to any part of the Derivative Works, in at least one
#           of the following places: within a NOTICE text file distributed
#           as part of the Derivative Works; within the Source form or
#           documentation, if provided along with the Derivative Works; or,
#           within a display generated by the Derivative Works, if and
#           wherever such third-party notices normally appear. The contents
#           of the NOTICE file are for informational purposes only and
#           do not modify the License. You may add Your own attribution
#           notices within Derivative Works that You distribute, alongside
#           or as an addendum to the NOTICE text from the Work, provided
#           that such additional attribution notices cannot be construed
#           as modifying the License.
#
#       You may add Your own copyright statement to Your modifications and
#       may provide additional or different license terms and conditions
#       for use, reproduction, or distribution of Your modifications, or
#       for any such Derivative Works as a whole, provided Your use,
#       reproduction, and distribution of the Work otherwise complies with
#       the conditions stated in this License.
#
#    5. Submission of Contributions. Unless You explicitly state otherwise,
#       any Contribution intentionally submitted for inclusion in the Work
#       by You to the Licensor shall be under the terms and conditions of
#       this License, without any additional terms or conditions.
#       Notwithstanding the above, nothing herein shall supersede or modify
#       the terms of any separate license agreement you may have executed
#       with Licensor regarding such Contributions.
#
#    6. Trademarks. This License does not grant permission to use the trade
#       names, trademarks, service marks, or product names of the Licensor,
#       except as required for reasonable and customary use in describing the
#       origin of the Work and reproducing the content of the NOTICE file.
#
#    7. Disclaimer of Warranty. Unless required by applicable law or
#       agreed to in writing, Licensor provides the Work (and each
#       Contributor provides its Contributions) on an "AS IS" BASIS,
#       WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
#       implied, including, without limitation, any warranties or conditions
#       of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
#       PARTICULAR PURPOSE. You are solely responsible for determining the
#       appropriateness of using or redistributing the Work and assume any
#       risks associated with Your exercise of permissions under this License.
#
#    8. Limitation of Liability. In no event and under no legal theory,
#       whether in tort (including negligence), contract, or otherwise,
#       unless required by applicable law (such as deliberate and grossly
#       negligent acts) or agreed to in writing, shall any Contributor be
#       liable to You for damages, including any direct, indirect, special,
#       incidental, or consequential damages of any character arising as a
#       result of this License or out of the use or inability to use the
#       Work (including but not limited to damages for loss of goodwill,
#       work stoppage, computer failure or malfunction, or any and all
#       other commercial damages or losses), even if such Contributor
#       has been advised of the possibility of such damages.
#
#    9. Accepting Warranty or Additional Liability. While redistributing
#       the Work or Derivative Works thereof, You may choose to offer,
#       and charge a fee for, acceptance of support, warranty, indemnity,
#       or other liability obligations and/or rights consistent with this
#       License. However, in accepting such obligations, You may act only
#       on Your own behalf and on Your sole responsibility, not on behalf
#       of any other Contributor, and only if You agree to indemnify,
#       defend, and hold each Contributor harmless for any liability
#       incurred by, or claims asserted against, such Contributor by reason
#       of your accepting any such warranty or additional liability.
#
#    END OF TERMS AND CONDITIONS
#
#    APPENDIX: How to apply the Apache License to your work.
#
#       To apply the Apache License to your work, attach the following
#       boilerplate notice, with the fields enclosed by brackets "[]"
#       replaced with your own identifying information. (Don't include
#       the brackets!)  The text should be enclosed in the appropriate
#       comment syntax for the file format. We also recommend that a
#       file or class name and description of purpose be included on the
#       same "printed page" as the copyright notice for easier
#       identification within third-party archives.
#
#    Copyright [yyyy] [name of copyright owner]
#
#    Licensed under the Apache License, Version 2.0 (the "License");
#    you may not use this file except in compliance with the License.
#    You may obtain a copy of the License at
#
#        http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS,
#    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#    See the License for the specific language governing permissions and
#    limitations under the License.
#
