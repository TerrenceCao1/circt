if [ "$#" -ne 1 ]; then
  echo "Usage: $0 <folder_path>"
  exit 1
fi

# Obtain folder
VERILOG_DIR="$1"

TOP_MODULE="top"
LIB_FILE="NangateOpenCellLibrary_typical.lib"
OUTPUT_DIR="out_dir"

# Validate folder
if [ ! -d "$VERILOG_DIR" ]; then
    echo "Error: Folder '$VERILOG_DIR' does not exist."
    exit 1
fi

# Check whether any .v files exist
shopt -s nullglob
VERILOG_FILES=("$VERILOG_DIR"/*.v)

if [ ${#VERILOG_FILES[@]} -eq 0 ]; then
    echo "No .v files found in '$VERILOG_DIR'."
    exit 1
fi

# Process each Verilog file
for VERILOG_FILE in "${VERILOG_FILES[@]}"; do
    echo "========================================"
    echo "Processing: $VERILOG_FILE"
    echo "========================================"

    bash ge_fo4.sh "$VERILOG_FILE" "$TOP_MODULE" "$LIB_FILE" "$OUTPUT_DIR"

    if [ $? -ne 0 ]; then
        echo "Error processing $VERILOG_FILE"
        continue
    fi

    echo "Successfully processed: $VERILOG_FILE"
done

echo "All Verilog files processed."
