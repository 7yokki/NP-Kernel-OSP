from PIL import Image
from pathlib import Path
source = Path('/home/ubuntu/npkernel/build/qemu-screen.ppm')
target = Path('/home/ubuntu/npkernel/build/qemu-screen.png')
Image.open(source).save(target)
print(target)
