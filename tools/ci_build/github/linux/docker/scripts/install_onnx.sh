#!/bin/bash
set -e
PYTHON_VER=$1

#Install ONNX
#5af210ca8a1c73aa6bae8754c9346ec54d0a756e is v1.2.3
#bae6333e149a59a3faa9c4d9c44974373dcf5256 is v1.3.0
#9e55ace55aad1ada27516038dfbdc66a8a0763db is v1.4.1
#7d7bc83d29a328233d3e8affa4c4ea8b3e3599ef is v1.5.0
#5c51f0dbbe88ee1536f17ee7bd462b2ab3772c52 is master
declare -A version2tag
version2tag+=(["5af210ca8a1c73aa6bae8754c9346ec54d0a756e"]="onnx123"
              ["bae6333e149a59a3faa9c4d9c44974373dcf5256"]="onnx130"
              ["9e55ace55aad1ada27516038dfbdc66a8a0763db"]="onnx141"
              ["7d7bc83d29a328233d3e8affa4c4ea8b3e3599ef"]="onnx150"
              ["5c51f0dbbe88ee1536f17ee7bd462b2ab3772c52"]="onnxtip")
for onnx_version in ${!version2tag[@]}; do
  if [ -z ${lastest_onnx_version+x} ]; then
    echo "first pass";
  else
    echo "deleting old onnx-${lastest_onnx_version}";
    /usr/bin/python${PYTHON_VER} -m pip uninstall -y onnx
  fi
  lastest_onnx_version=$onnx_version
  aria2c -q -d /tmp/src  https://github.com/onnx/onnx/archive/$onnx_version.tar.gz
  tar -xf /tmp/src/onnx-$onnx_version.tar.gz -C /tmp/src
  cd /tmp/src/onnx-$onnx_version
  git clone https://github.com/pybind/pybind11.git third_party/pybind11
  /usr/bin/python${PYTHON_VER} -m pip install .
  mkdir -p /data/onnx/${version2tag[$onnx_version]}
  backend-test-tools generate-data -o /data/onnx/${version2tag[$onnx_version]}
  echo $onnx_version":"${version2tag[$onnx_version]} >> /data/onnx/version2tag
done
