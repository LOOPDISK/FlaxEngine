// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.GUI.Dialogs;
using FlaxEngine;
using FlaxEngine.GUI;

namespace FlaxEditor.Content.Import
{
    /// <summary>
    /// Dialog that shows source files that could not be found during reimport, with options to browse manually or cancel.
    /// </summary>
    /// <seealso cref="FlaxEditor.GUI.Dialogs.Dialog" />
    public class MissingImportFilesDialog : Dialog
    {
        private Action<bool> _onComplete;

        /// <summary>
        /// Initializes a new instance of the <see cref="MissingImportFilesDialog"/> class.
        /// </summary>
        /// <param name="missingFiles">List of (fileName, assetName) pairs for files that could not be found.</param>
        /// <param name="onComplete">Callback invoked with true if the user chose to browse manually, false if cancelled.</param>
        public MissingImportFilesDialog(List<KeyValuePair<string, string>> missingFiles, Action<bool> onComplete)
        : base("Missing Source Files")
        {
            _onComplete = onComplete;

            const float TotalWidth = 500;
            const float ButtonsHeight = 24;
            const float ButtonsMargin = 8;

            // Header
            var headerLabel = new Label
            {
                Text = "The following source files could not be found:",
                HorizontalAlignment = TextAlignment.Near,
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Offsets = new Margin(10, 10, 10, 25),
                Parent = this,
            };

            // Scrollable file list
            float listTop = headerLabel.Bottom + 5;
            float listHeight = Mathf.Clamp(missingFiles.Count * 22 + 10, 50, 250);

            var panel = new Panel(ScrollBars.Vertical)
            {
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Offsets = new Margin(10, 10, listTop, listHeight),
                Parent = this,
            };

            var layout = new VerticalPanel
            {
                AnchorPreset = AnchorPresets.HorizontalStretchTop,
                Offsets = Margin.Zero,
                Parent = panel,
                Spacing = 2,
                Margin = new Margin(4, 4, 4, 4),
            };

            foreach (var missing in missingFiles)
            {
                new Label
                {
                    Text = string.Format("{0}  (Asset: {1})", missing.Key, missing.Value),
                    HorizontalAlignment = TextAlignment.Near,
                    AnchorPreset = AnchorPresets.HorizontalStretchTop,
                    Height = 20,
                    Parent = layout,
                };
            }

            // Buttons
            const float BrowseButtonWidth = 130;
            const float CancelButtonWidth = 90;

            var browseButton = new Button
            {
                Text = "Browse Manually",
                AnchorPreset = AnchorPresets.BottomRight,
                Offsets = new Margin(-BrowseButtonWidth - ButtonsMargin, BrowseButtonWidth, -ButtonsHeight - ButtonsMargin, ButtonsHeight),
                Parent = this,
            };
            browseButton.Clicked += OnSubmit;

            var cancelButton = new Button
            {
                Text = "Cancel All",
                AnchorPreset = AnchorPresets.BottomRight,
                Offsets = new Margin(-BrowseButtonWidth - ButtonsMargin - CancelButtonWidth - ButtonsMargin, CancelButtonWidth, -ButtonsHeight - ButtonsMargin, ButtonsHeight),
                Parent = this,
            };
            cancelButton.Clicked += OnCancel;

            float totalHeight = listTop + listHeight + ButtonsHeight + ButtonsMargin * 3;
            _dialogSize = new Float2(TotalWidth, totalHeight);
        }

        /// <inheritdoc />
        public override void OnSubmit()
        {
            var callback = _onComplete;
            _onComplete = null;
            base.OnSubmit();
            callback?.Invoke(true);
        }

        /// <inheritdoc />
        public override void OnCancel()
        {
            var callback = _onComplete;
            _onComplete = null;
            base.OnCancel();
            callback?.Invoke(false);
        }

        /// <inheritdoc />
        protected override bool CanCloseWindow(ClosingReason reason)
        {
            // Handle X button close as cancel
            if (reason == ClosingReason.User && _onComplete != null)
            {
                var callback = _onComplete;
                _onComplete = null;
                callback.Invoke(false);
            }
            return true;
        }

        /// <inheritdoc />
        protected override void SetupWindowSettings(ref CreateWindowSettings settings)
        {
            base.SetupWindowSettings(ref settings);
            settings.MinimumSize = new Float2(300, 150);
            settings.HasSizingFrame = true;
        }
    }
}
